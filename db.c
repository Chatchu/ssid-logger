/*
ssid-logger is a simple software to log SSID you encouter in your vicinity
Copyright © 2020 solsTiCe d'Hiver
*/
#include <stdio.h>
#include <stdlib.h>
#include <sqlite3.h>
#include <string.h>
#include <math.h>

#include "ap_info.h"
#include "gps_thread.h"
#include "parsers.h"

int init_beacon_db(const char *db_file, sqlite3 **db)
{
  int ret;
  if ((ret = sqlite3_open(db_file, db)) != SQLITE_OK) {
    fprintf(stderr, "Error: %s\n", sqlite3_errmsg(*db));
    sqlite3_close(*db);
    return ret;
  }

  char *sql;
  sql = "create table if not exists authmode("
    "id integer not null primary key,"
    "mode text"
    ");";
  if ((ret = sqlite3_exec(*db, sql, NULL, 0, NULL)) != SQLITE_OK) {
    fprintf(stderr, "Error: %s\n", sqlite3_errmsg(*db));
    sqlite3_close(*db);
    return ret;
  }
  sql = "create table if not exists ap("
    "id integer not null primary key,"
    "bssid text not null,"
    "ssid text not null,"
    "unique (bssid, ssid)"
    ");";
  if ((ret = sqlite3_exec(*db, sql, NULL, 0, NULL)) != SQLITE_OK) {
    fprintf(stderr, "Error: %s\n", sqlite3_errmsg(*db));
    sqlite3_close(*db);
    return ret;
  }
  sql = "create table if not exists beacon("
    "ts integer,"
    "ap integer,"
    "channel integer,"
    "rssi integer,"
    "lat float,"
    "lon float,"
    "alt float,"
    "acc float,"
    "authmode integer,"
    "foreign key(ap) references ap(id),"
    "foreign key(authmode) references authmode(id)"
    ");";
  if ((ret = sqlite3_exec(*db, sql, NULL, 0, NULL)) != SQLITE_OK) {
    fprintf(stderr, "Error: %s\n", sqlite3_errmsg(*db));
    sqlite3_close(*db);
    return ret;
  }
  sql = "pragma synchronous = normal;";
  if ((ret = sqlite3_exec(*db, sql, NULL, 0, NULL)) != SQLITE_OK) {
    fprintf(stderr, "Error: %s\n", sqlite3_errmsg(*db));
    sqlite3_close(*db);
    return ret;
  }
  sql = "pragma temp_store = 2;"; // to store temp table and indices in memory
  if ((ret = sqlite3_exec(*db, sql, NULL, 0, NULL)) != SQLITE_OK) {
    fprintf(stderr, "Error: %s\n", sqlite3_errmsg(*db));
    sqlite3_close(*db);
    return ret;
  }
  sql = "pragma journal_mode = off;"; // disable journal for rollback (we don't use this)
  if ((ret = sqlite3_exec(*db, sql, NULL, 0, NULL)) != SQLITE_OK) {
    fprintf(stderr, "Error: %s\n", sqlite3_errmsg(*db));
    sqlite3_close(*db);
    return ret;
  }
  sql = "pragma foreign_keys = on;"; // this needs to be turn on
  if ((ret = sqlite3_exec(*db, sql, NULL, 0, NULL)) != SQLITE_OK) {
    fprintf(stderr, "Error: %s\n", sqlite3_errmsg(*db));
    sqlite3_close(*db);
    return ret;
  }

  return 0;
}

int64_t search_authmode(const char *authmode, sqlite3 *db)
{
  char *sql;
  sqlite3_stmt *stmt;
  int64_t authmode_id = 0, ret;

  // look for an existing ap_info in the db
  sql = "select id from authmode where mode=?;";
  if ((ret = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) != SQLITE_OK) {
    fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
    return ret * -1;
  } else {
    if ((ret = sqlite3_bind_text(stmt, 1, authmode, -1, NULL)) != SQLITE_OK) {
      fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
      return ret * -1;
    }

    while ((ret = sqlite3_step(stmt)) != SQLITE_DONE) {
      if (ret == SQLITE_ERROR) {
        fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
        break;
      } else if (ret == SQLITE_ROW) {
        authmode_id = sqlite3_column_int64(stmt, 0);
      } else {
        fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
        break;
      }
    }
    sqlite3_finalize(stmt);
  }
  return authmode_id;
}

int64_t insert_authmode(const char *authmode, sqlite3 *db)
{
    // insert the authmode into the db
  int64_t ret, authmode_id = 0;
  char sql[128];

  authmode_id = search_authmode(authmode, db);
  if (!authmode_id) {
    snprintf(sql, 128, "insert into authmode (mode) values (\"%s\");", authmode);
    if ((ret = sqlite3_exec(db, sql, NULL, 0, NULL)) != SQLITE_OK) {
      fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
      sqlite3_close(db);
      return ret * -1;
    }
    authmode_id = search_authmode(authmode, db);
  }

  return authmode_id;
}

int64_t search_ap(struct ap_info ap, sqlite3 *db)
{
  char *sql;
  sqlite3_stmt *stmt;
  int64_t ap_id = 0, ret;

  // look for an existing ap_info in the db
  sql = "select id from ap where bssid=? and ssid=?;";
  if ((ret = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) != SQLITE_OK) {
    fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
    return ret * -1;
  } else {
    if ((ret = sqlite3_bind_text(stmt, 1, ap.bssid, -1, NULL)) != SQLITE_OK) {
      fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
      return ret * -1;
    }
    if ((ret = sqlite3_bind_text(stmt, 2, ap.ssid, -1, NULL)) != SQLITE_OK) {
      fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
      return ret * -1;
    }

    while ((ret = sqlite3_step(stmt)) != SQLITE_DONE) {
      if (ret == SQLITE_ERROR) {
        fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
        break;
      } else if (ret == SQLITE_ROW) {
        ap_id = sqlite3_column_int64(stmt, 0);
      } else {
        fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
        break;
      }
    }
    sqlite3_finalize(stmt);
  }
  return ap_id;
}

int64_t insert_ap(struct ap_info ap, sqlite3 *db)
{
    // insert the ap_info into the db
  int64_t ret, ap_id = 0;
  char sql[128];

  ap_id = search_ap(ap, db);
  if (!ap_id) {
    // if ever the ssid is longer than 32 chars, it is truncated at 128-18-length of string below
    snprintf(sql, 128, "insert into ap (bssid, ssid) values (\"%s\", \"%s\");", ap.bssid, ap.ssid);
    if ((ret = sqlite3_exec(db, sql, NULL, 0, NULL)) != SQLITE_OK) {
      fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
      sqlite3_close(db);
      return ret * -1;
    }
    ap_id = search_ap(ap, db);
  }

  return ap_id;
}

int insert_beacon(struct ap_info ap, struct gps_loc gloc, sqlite3 *db)
{
  int64_t ap_id, authmode_id;
  int ret;

  ap_id = insert_ap(ap, db);
  char *authmode = authmode_from_crypto(ap.rsn, ap.msw, ap.ess, ap.privacy, ap.wps);
  authmode_id = insert_authmode(authmode, db);
  free(authmode);

  char sql[256];
  snprintf(sql, 256, "insert into beacon (ts, ap, channel, rssi, lat, lon, alt, acc, authmode)"
    "values (%lu, %lu, %u, %d, %f, %f, %f, %f, %lu);",
    gloc.ftime.tv_sec, ap_id, ap.channel, ap.rssi, gloc.lat, gloc.lon, isnan(gloc.alt) ? 0.0 : gloc.alt, gloc.acc, authmode_id);
  if ((ret = sqlite3_exec(db, sql, NULL, 0, NULL)) != SQLITE_OK) {
    fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
    sqlite3_close(db);
    return ret * -1;
  }

  return 0;
}

int begin_txn(sqlite3 *db)
{
  int ret;
  char sql[32];

  snprintf(sql, 32, "begin transaction;");
  if ((ret = sqlite3_exec(db, sql, NULL, 0, NULL)) != SQLITE_OK) {
    fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
    sqlite3_close(db);
    return ret * -1;
  }

  return 0;
}

int commit_txn(sqlite3 *db)
{
  int ret;
  char sql[32];

  snprintf(sql, 32, "commit transaction;");
  if ((ret = sqlite3_exec(db, sql, NULL, 0, NULL)) != SQLITE_OK) {
    fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
    sqlite3_close(db);
    return ret * -1;
  }

  return 0;
}
