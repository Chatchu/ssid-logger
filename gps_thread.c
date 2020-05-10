/*
ssid-logger is a simple software to log SSID you encouter in your vicinity
Copyright © 2020 solsTiCe d'Hiver
*/
#include <gps.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>

#include "gps_thread.h"

int gps_thread_init_result;
pthread_mutex_t mutex_gtr;
pthread_cond_t cv_gtr;
bool has_gps_got_fix;

void cleanup_gps_data(void *arg)
{
  struct gps_data_t *gps_data;
  gps_data = (struct gps_data_t *) arg;

  gps_stream(gps_data, WATCH_DISABLE, NULL);
  gps_close(gps_data);

  return;
}

static inline int update_gloc(struct gps_data_t gps_data)
{
  // update global gloc gps location
  gloc.lat = gps_data.fix.latitude;
  gloc.lon = gps_data.fix.longitude;
  #if GPSD_API_MAJOR_VERSION >= 9
  gloc.alt = isnan(gps_data.fix.altMSL) ? 0.0 : gps_data.fix.altMSL;
  gloc.ftime = gps_data.fix.time;
  if (!isnan(gps_data.fix.eph)) {
    gloc.acc = gps_data.fix.eph;
  } else {
    gloc.acc = 0.0;
  }
  #else
  gloc.alt = isnan(gps_data.fix.altitude) ? 0.0 : gps_data.fix.altitude;
  gloc.ftime.tv_sec = (time_t)gps_data.fix.time;
  if (!isnan(gps_data.fix.epx) && !isnan(gps_data.fix.epy)) {
    gloc.acc = (gps_data.fix.epx + gps_data.fix.epy)/2;
  } else {
    gloc.acc = 0.0;
  }
  #endif
  // we use the system clock to avoid problem if
  // the system clock and the gps time are not in sync
  // gloc.ctime is only used for relative timing
  clock_gettime(CLOCK_MONOTONIC, &gloc.ctime);

  return 0;
}

// helper thread that repeatedly retrieve gps coord. from the gpsd daemon
void *retrieve_gps_data(void *arg)
{
  struct gps_data_t gps_data;
  pthread_mutex_t mutex_gloc;
  enum _option_gps *option_gps;

  option_gps = (enum _option_gps *)arg;
  if (*option_gps == GPS_LOG_ZERO) {
    // don't use gpsd
    pthread_mutex_lock(&mutex_gtr);
    gps_thread_init_result = 1;
    pthread_cond_signal(&cv_gtr);
    pthread_mutex_unlock(&mutex_gtr);
    return NULL;
  }

  if (gps_open(GPSD_HOST, GPSD_PORT, &gps_data) == -1) {
    // error connecting to gpsd
    fprintf(stderr, "Error(gpsd): %s\n", gps_errstr(errno));
    pthread_mutex_lock(&mutex_gtr);
    gps_thread_init_result = 2;
    pthread_cond_signal(&cv_gtr);
    pthread_mutex_unlock(&mutex_gtr);
    return NULL;
  }
  gps_stream(&gps_data, WATCH_ENABLE | WATCH_JSON, NULL);

  pthread_mutex_lock(&mutex_gtr);
  gps_thread_init_result = 0;
  pthread_cond_signal(&cv_gtr);
  pthread_mutex_unlock(&mutex_gtr);

  // push clean up code when thread is cancelled
  pthread_cleanup_push(cleanup_gps_data, (void *) (&gps_data));

  pthread_mutex_init(&mutex_gloc, NULL);

  int status, ret;

  while (1) {
    gloc.lat = gloc.lon = gloc.alt = gloc.acc = 0.0;
    // wait at most for 1 second to receive data
    if (gps_waiting(&gps_data, 1000000)) {
      #if GPSD_API_MAJOR_VERSION >= 7
      ret = gps_read(&gps_data, NULL, 0);
        #if GPSD_API_MAJOR_VERSION >= 10
        status = gps_data.fix.status;
        #else
        status = gps_data.status;
        #endif
      #else
      ret = gps_read(&gps_data);
      status = gps_data.status;
      #endif
      if ((ret > 0) && gps_data.set && (status == STATUS_FIX)
          && ((gps_data.fix.mode == MODE_2D) || (gps_data.fix.mode == MODE_3D ))
          && !isnan(gps_data.fix.latitude)
          && !isnan(gps_data.fix.longitude)) {
        pthread_mutex_lock(&mutex_gloc);
        if (!has_gps_got_fix) has_gps_got_fix = true;
        update_gloc(gps_data);
        pthread_mutex_unlock(&mutex_gloc);
      }
    }
    usleep(500000);
    pthread_testcancel();
  }

  pthread_cleanup_pop(1);

  return NULL;
}
