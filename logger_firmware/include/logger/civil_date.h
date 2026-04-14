#ifndef LOGGER_CIVIL_DATE_H
#define LOGGER_CIVIL_DATE_H

/*
 * Howard Hinnant's civil-date algorithms.
 * See: https://howardhinnant.github.io/date_algorithms.html
 *
 * Both functions are `static inline` so each translation unit gets its
 * own copy — the compiler folds them away at -O1 and above.
 */

#include <stdint.h>

/* year/month/day → epoch days (Unix epoch: 1970-01-01 == 0) */
static inline int64_t logger_days_from_civil(int year, int month, int day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = (unsigned)(year - era * 400);
  const unsigned doy =
      (153u * (unsigned)(month + (month > 2 ? -3 : 9)) + 2u) / 5u +
      (unsigned)day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* epoch days → year/month/day (Unix epoch: 1970-01-01 == 0) */
static inline void logger_civil_from_days(int64_t z, int *year, unsigned *month,
                                          unsigned *day) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - (era * 146097));
  const unsigned yoe =
      (doe - (doe / 1460) + (doe / 36524) - (doe / 146096)) / 365;
  int y = (int)yoe + (int)(era * 400);
  const unsigned doy = doe - (365u * yoe + (yoe / 4) - (yoe / 100));
  const unsigned mp = (5u * doy + 2u) / 153u;
  const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
  const unsigned m = (mp < 10u) ? (mp + 3u) : (mp - 9u);
  y += m <= 2u;
  *year = y;
  *month = m;
  *day = d;
}

#endif /* LOGGER_CIVIL_DATE_H */
