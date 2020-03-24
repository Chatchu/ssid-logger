#ifndef GPS_THREAD_H
#define GPS_THREAD_H

#include <time.h>

struct gps_loc {
  double lat;
  double lon;
  double alt;
  double acc;
  struct timespec ctime;
  struct timespec ftime;
} gloc;

void *retrieve_gps_data(void *arg);

#endif
