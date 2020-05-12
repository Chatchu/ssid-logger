/*
ssid-logger is a simple software to log SSID you encouter in your vicinity
Copyright © 2020 solsTiCe d'Hiver
*/
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#include "radiotap_iter.h"
#include "parsers.h"
#include "gps_thread.h"
#include "logger_thread.h"

#define MAX_AUTHMODE_LEN 192

//static const char *CIPHER_SUITE_SELECTORS[] =
//    { "Use group cipher suite", "WEP-40", "TKIP", "", "CCMP", "WEP-104", "BIP" };

void free_cipher_suite(struct cipher_suite *cs)
{
  if (cs == NULL)
    return;

  uint16_t pcsc = cs->pairwise_cipher_count;
  for (int i = 0; i < pcsc; i++) {
    free(cs->pairwise_cipher_suite[i]);
  }
  free(cs->pairwise_cipher_suite);

  uint16_t akmsc = cs->akm_cipher_count;
  for (int i = 0; i < akmsc; i++) {
    free(cs->akm_cipher_suite[i]);
  }
  free(cs->akm_cipher_suite);

  free(cs);
  cs = NULL;

  return;
}

struct cipher_suite *parse_cipher_suite(uint8_t * start)
{
  struct cipher_suite *cs = malloc(sizeof(struct cipher_suite));

  memcpy(cs->group_cipher_suite, start, 4);

  uint16_t pcsc = cs->pairwise_cipher_count = *(start + 4);
  cs->pairwise_cipher_suite = malloc(pcsc * sizeof(uint8_t *));
  for (int i = 0; i < pcsc; i++) {
    cs->pairwise_cipher_suite[i] = malloc(4 * sizeof(uint8_t));
    memcpy(cs->pairwise_cipher_suite[i], start + 4 + 2 + i * 4, 4);
  }

  uint16_t akmsc = cs->akm_cipher_count = *(start + 4 + 2 + pcsc * 4);
  cs->akm_cipher_suite = malloc(akmsc * sizeof(uint8_t *));
  for (int i = 0; i < akmsc; i++) {
    cs->akm_cipher_suite[i] = malloc(4 * sizeof(uint8_t));
    memcpy(cs->akm_cipher_suite[i], start + 4 + 2 + pcsc * 4 + 2 + i * 4, 4);
  }
  return cs;
}

int8_t parse_radiotap_header(const uint8_t * packet, uint16_t * freq, int8_t * rssi)
{
  // parse radiotap header to get frequency and rssi
  // returns radiotap header size or -1 on error
  struct ieee80211_radiotap_header *rtaphdr;
  rtaphdr = (struct ieee80211_radiotap_header *) (packet);
  int8_t offset = (int8_t) rtaphdr->it_len;

  struct ieee80211_radiotap_iterator iter;
  //uint16_t flags = 0;
  int8_t r;

  static const struct radiotap_align_size align_size_000000_00[] = {
    [0] = {.align = 1,.size = 4, },
    [52] = {.align = 1,.size = 4, },
  };

  static const struct ieee80211_radiotap_namespace vns_array[] = {
    {
     .oui = 0x000000,
     .subns = 0,
     .n_bits = sizeof(align_size_000000_00),
     .align_size = align_size_000000_00,
      },
  };

  static const struct ieee80211_radiotap_vendor_namespaces vns = {
    .ns = vns_array,
    .n_ns = sizeof(vns_array) / sizeof(vns_array[0]),
  };

  int err =
      ieee80211_radiotap_iterator_init(&iter, rtaphdr, rtaphdr->it_len,
                                       &vns);
  if (err) {
    printf("Error: malformed radiotap header (init returned %d)\n", err);
    return -1;
  }

  *freq = 0;
  *rssi = 0;
  // iterate through radiotap filed and look for frequency and rssi
  while (!(err = ieee80211_radiotap_iterator_next(&iter))) {
    if (iter.this_arg_index == IEEE80211_RADIOTAP_CHANNEL) {
      assert(iter.this_arg_size == 4);  // XXX: why ?
      *freq = iter.this_arg[0] + (iter.this_arg[1] << 8);
      //flags = iter.this_arg[2] + (iter.this_arg[3] << 8);
    }
    if (iter.this_arg_index == IEEE80211_RADIOTAP_DBM_ANTSIGNAL) {
      r = (int8_t) * iter.this_arg;
      if (r != 0)
        *rssi = r;              // XXX: why do we get multiple dBm_antSignal with 0 value after the first one ?
    }
    if (*freq != 0 && *rssi != 0)
      break;
  }
  return offset;
}

struct ap_info *parse_beacon_frame(const uint8_t *packet, uint32_t packet_len, int8_t offset)
{
  struct ap_info *ap = malloc(sizeof(struct ap_info));

  // parse the beacon frame to look for BSSID and Information Element we need (ssid, crypto, wps)
  // BSSID
  const uint8_t *bssid_addr = packet + offset + 2 + 2 + 6 + 6;   // FC + duration + DA + SA
  sprintf(ap->bssid, "%02X:%02X:%02X:%02X:%02X:%02X", bssid_addr[0],
    bssid_addr[1], bssid_addr[2], bssid_addr[3], bssid_addr[4], bssid_addr[5]);

  // Capability Info
  const uint8_t *ci_addr = bssid_addr + 6 + 2 + 8 + 2;
  uint16_t ci_fields;
  memcpy(&ci_fields, ci_addr, sizeof(ci_fields));
  ap->ess = (bool) (ci_fields & 0x0001);
  ap->privacy = (bool) ((ci_fields & 0x0010) >> 4);

  ap->ssid = NULL;
  uint8_t *ie = (uint8_t *) ci_addr + 2;
  uint8_t ie_len = *(ie + 1);
  ap->channel = 0, ap->ssid_len = 0;
  ap->wps = false/*, utf8_ssid = false*/;

  ap->rsn = NULL;
  ap->msw = NULL;
  // iterate over Information Element to look for SSID and RSN crypto and MicrosoftWPA
  while (ie < packet + packet_len) {
    if ((ie + ie_len + 2 <= packet + packet_len)) {     // just double check that this is an IE with length inside packet
      switch (*ie) {
      case 0:                  // SSID aka IE with id 0
        ap->ssid_len = *(ie + 1);
        ap->ssid = (char *) malloc((ap->ssid_len + 1) * sizeof(uint8_t));        // AP name
        snprintf(ap->ssid, ap->ssid_len + 1, "%s", ie + 2);
        break;
      case 3:                  // IE with id 3 is DS parameter set ~= channel
        ap->channel = *(ie + 2);
        break;
      case 48:                 // parse RSN IE
        ap->rsn = parse_cipher_suite(ie + 4);
        break;
      case 127:                // Extended Capabilities IE
        if (ie_len >= 7) {
          //utf8_ssid = (bool) (*(ie + 1 + 7) & 0x01);
        }
        break;
      case 221:
        if (memcmp(ie + 2, MS_OUI "\001\001", 5) == 0) {
          // parse MicrosoftWPA IE
          ap->msw = parse_cipher_suite(ie + 8);
        } else if (memcmp(ie + 2, WPS_ID, 4) == 0) {
          ap->wps = true;
        }
        break;
      }
    }
    ie = ie + ie_len + 2;
    ie_len = *(ie + 1);
  }
  return ap;
}

char *authmode_from_crypto(struct cipher_suite *rsn, struct cipher_suite *msw,
                            bool ess, bool privacy, bool wps)
{
  char authmode[MAX_AUTHMODE_LEN];
  authmode[0] = '\0';           // this is needed for strcat to work
  uint8_t last_byte;
  size_t length = MAX_AUTHMODE_LEN;

  if (msw != NULL) {
    strncat(authmode, "[WPA-", length);
    length -= 5;
    last_byte = (uint8_t) msw->akm_cipher_suite[0][3];
    switch (last_byte) {
    case 1:
      strncat(authmode, "EAP-", length);
      length -= 4;
      break;
    case 2:
      strncat(authmode, "PSK-", length);
      length -= 4;
      break;
    }
    bool first = true;
    for (int i = 0; i < msw->pairwise_cipher_count; i++) {
      last_byte = (uint8_t) msw->pairwise_cipher_suite[i][3];
      if (!first) {
        strncat(authmode, "+", length);
        length -= 1;
      } else {
        first = false;
      }
      switch (last_byte) {
      case 2:
        strncat(authmode, "TKIP", length);
        length -= 4;
        break;
      case 4:
        strncat(authmode, "CCMP", length);
        length -= 4;
        break;
      case 1:
        strncat(authmode, "WEP-40", length);
        length -= 6;
        break;
      case 5:
        strncat(authmode, "WEP-104", length);
        length -= 7;
        break;
      }
    }
    strncat(authmode, "]", length);
    length -= 1;
  }
  if (rsn != NULL) {
    strncat(authmode, "[WPA2-", length);
    length -= 6;
    last_byte = (uint8_t) rsn->akm_cipher_suite[0][3];
    switch (last_byte) {
    case 1:
      strncat(authmode, "EAP-", length);
      length -= 4;
      break;
    case 2:
      strncat(authmode, "PSK-", length);
      length -= 4;
      break;
    }
    bool first = true;
    for (int i = 0; i < rsn->pairwise_cipher_count; i++) {
      last_byte = (uint8_t) rsn->pairwise_cipher_suite[i][3];
      if (!first) {
        strncat(authmode, "+", length);
        length -= 1;
      } else {
        first = false;
      }
      switch (last_byte) {
      case 2:
        strncat(authmode, "TKIP", length);
        length -= 4;
        break;
      case 4:
        strncat(authmode, "CCMP", length);
        length -= 4;
        break;
      case 1:
        strncat(authmode, "WEP-40", length);
        length -= 6;
        break;
      case 5:
        strncat(authmode, "WEP-104", length);
        length -= 7;
        break;
      }
    }
    strncat(authmode, "]", length);
    length -= 1;
  }
  if (!rsn && !msw && privacy) {
    strncat(authmode, "[WEP]", length);
    length -= 5;
  }
  if (ess) {
    strncat(authmode, "[ESS]", length);
    length -= 5;
  }
  if (wps) {
    strncat(authmode, "[WPS]", length);
    length -= 5;
  }

  return strndup(authmode, MAX_AUTHMODE_LEN-length);
}

char *ap_to_str(struct ap_info ap, struct gps_loc gloc)
{
  char tmp[384], tail[64], firstseen[21];
  char *authmode, *ap_str;

  authmode = authmode_from_crypto(ap.rsn, ap.msw, ap.ess, ap.privacy, ap.wps);
  strftime(firstseen, 20, "%Y-%m-%d %H:%M:%S", gmtime(&gloc.ftime.tv_sec));
  sprintf(tail, "%d,%d,%-2.6f,%-2.6f,%-2.6f,%-2.6f,WIFI", ap.channel, ap.rssi, gloc.lat,
    gloc.lon, gloc.alt, gloc.acc);

  tmp[0] = '\0';
  strncat(tmp, ap.bssid, 18);
  strcat(tmp, ",");
  strncat(tmp, ap.ssid, 64); // that's the double allowed size by the 802.11 standard
  strcat(tmp, ",");
  strncat(tmp, authmode, MAX_AUTHMODE_LEN);
  strcat(tmp, ",");
  strncat(tmp, firstseen, 21);
  strcat(tmp, ",");
  strncat(tmp, tail, 55);
  free(authmode);

  ap_str = malloc(strlen(tmp)+1);
  strncpy(ap_str, tmp, strlen(tmp)+1);

  return ap_str;

//MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type
//A4:3E:51:XX:XX:XX,Livebox-XXXX,[WPA-PSK-CCMP+TKIP] [WPA2-PSK-CCMP+TKIP][ESS],2020-02-15 17:52:51,6,-78,50.0000000000,-3.0000000000,19.308001,0,WIFI
}

int parse_os_release(char **os_name, char **os_version)
{
  char buf[BUFSIZ], *p;
  char *path = strdup("/etc/os-release");
  FILE *fp = fopen(path, "r");

  if (fp == NULL) {
    free(path);
    path = strdup("/usr/lib/os-release");
    fp = fopen(path, "r");
  }
  free(path);
  if (fp == NULL) {
    return -1;
  }
  while (fgets(buf, sizeof(buf), fp)) {
    char *value, *q;

    // ignore comments
    if (buf[0] == '#') {
      continue;
    }

    // split into name=value
    p = strchr(buf, '=');
    if (!p) {
      continue;
    }
    *p++ = 0;

    value = p;
    q = p;
    while (*p) {
      if (*p == '\\') {
        ++p;
        if (!*p) {
          break;
        }
        *q++ = *p++;
      } else if (*p == '\'' || *p == '"' ||
          *p == '\n') {
        ++p;
      } else {
        *q++ = *p++;
      }
    }
    *q = 0;

    if (!strcmp(buf, "ID")) {
      *os_name = strdup(value);
    } else if (!strcmp(buf, "VERSION_ID")) {
      *os_version = strdup(value);
    }
  }
  fclose(fp);

  return 0;
}
