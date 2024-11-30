//
// $ cc simd_parse_time.c -Wall -Wextra -fsanitize=address,undefined -march=native -fopenmp -ggdb -O0
//

#include <stdint.h>
#include <assert.h>

#include <stdio.h>
#include <time.h>

#include <x86intrin.h>

struct sse_parsed_time {
  char *error;
  uint32_t time; // Unix time

  uint32_t year;                // [0-9999]
  uint32_t month;               // [0-11]
  uint32_t mday;                // [1-31]
  uint32_t hour;                // [0-23]
  uint32_t minute;              // [0-59]
  uint32_t second;              // [0-59]
};

static const uint32_t mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
static const uint32_t mdays_cumulative[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
static inline uint64_t is_leap_year(uint32_t year) {
  return (year % 4 == 0) & ((year % 100 != 0) | (year % 400 == 0));
}

static inline int leap_days(int y1, int y2) {
  --y1;
  --y2;
  return (y2 / 4 - y1 / 4) - (y2 / 100 - y1 / 100) + (y2 / 400 - y1 / 400);
}

static struct sse_parsed_time sse_parse_time(char str[static 16])
{
  __m128i v = _mm_loadu_si128((const __m128i *)str);
  v = _mm_sub_epi8(v, _mm_set1_epi8('0'));

  __m128i limit = _mm_setr_epi8(9, 9, 9, 9, // year
                                1, 9,       // month
                                3, 9,       // day
                                2, 9,       // hour
                                5, 9,       // minute
                                5, 9,       // second
                                -1, -1);
  __m128i abide_by_limits = _mm_subs_epu8(v, limit); // must be zero

  __m128i weights = _mm_setr_epi8(10, 1, 10, 1, // year
                                  10, 1,        // month
                                  10, 1,        // day
                                  10, 1,        // hour
                                  10, 1,        // minute
                                  10, 1,        // second
                                  0, 0);
  v = _mm_maddubs_epi16(v, weights);

  __m128i limit16 = _mm_setr_epi16(99, 99,  // year
                                   12,      // month
                                   31,      // day
                                   23,      // hour
                                   59,      // minute
                                   59,      // second
                                   -1);
  __m128i abide_by_limits16 = _mm_subs_epu16(v, limit16); // must be zero

  __m128i limits = _mm_or_si128(abide_by_limits, abide_by_limits16);
  if (!_mm_test_all_zeros(limits, limits)) {
    return (struct sse_parsed_time) { .error = "Date not valid.", };
  }

  uint64_t hi = (uint64_t)_mm_extract_epi64(v, 1);
  uint64_t lo = (uint64_t)_mm_extract_epi64(v, 0);

  uint32_t yr = ((lo >> 0)  & 0xFF) * 100 +
                ((lo >> 16) & 0xFF) * 1;
  uint32_t mo = ((lo >> 32)  & 0xFF) - 1;
  uint32_t mday = (lo >> 48)  & 0xFF;
  uint32_t hour   = (hi >> 0) & 0xFF;
  uint32_t minute = (hi >> 16) & 0xFF;
  uint32_t second = (hi >> 32) & 0xFF;

  if (yr < 1970) {
    return (struct sse_parsed_time) {
      .error = "Year less than 1970.",
      .year = yr,
      .month = mo,
      .mday = mday,
      .hour = hour,
      .minute = minute,
      .second = second,
    };
  }

  _Bool leap_yr = (_Bool)is_leap_year(yr);

  if (mday > mdays[mo]) {
    if (mo == 1 && leap_yr) {
      if (mday != 29) {
        return (struct sse_parsed_time) { .error = "More days than valid in month.", };
      }
    }
    else {
      return (struct sse_parsed_time) { .error = "More days than valid in month.", };
    }
  }

  assert(yr >= 1970);
  uint64_t days = 365 * (yr - 1970) + (uint64_t)leap_days(1970, yr);
  days += mdays_cumulative[mo];
  days += leap_yr & (mo > 1);
  days += mday - 1;

  uint64_t time64 = second +
                    minute * 60 +
                    hour   * 60 * 60 +
                    days   * 60 * 60 * 24;
  uint32_t time = (uint32_t)time64;
  assert(time64 == time && "Overflow");

  return (struct sse_parsed_time) {
    .time = time,
    .year = yr,   .month  = mo,     .mday   = mday,
    .hour = hour, .minute = minute, .second = second,
  };
}


static void format_time(char buffer[static 16], time_t rawtime)
{
  struct tm timeinfo;
  gmtime_r(&rawtime, &timeinfo);
  size_t len = strftime(buffer, 16, "%Y%m%d%H%M%S", &timeinfo);
  assert(len != 0);
}


#ifdef TEST_ALL
#include <omp.h>

#define EXPECT(got, expected)                                           \
  if ((got) != (expected)) {                                            \
    printf("When parsing time %.*s: Got: %d, Expected: %d\n", (int)sizeof(buf), buf, (got), (expected)); \
    _Pragma("omp atomic")                                               \
      errors += 1;                                                      \
  }

_Bool test_all_valid(void) {
  int errors = 0;

  #pragma omp parallel for shared(errors)
  for (time_t rawtime = 0; rawtime < UINT32_MAX - 1; rawtime += 1) {
    char buf[16];
    format_time(buf, rawtime);

    struct sse_parsed_time t = sse_parse_time(buf);
    if (t.error) {
      fprintf(stderr, "Error parsing time %.*s: %s\n", 16, buf, t.error);
      #pragma omp atomic
      errors += 1;
    }
    else {
      if (t.time != rawtime) {
        printf("When parsing time %.*s: Got: %u, Expected: %lu\n", (int)sizeof(buf), buf, t.time, rawtime);
        #pragma omp atomic
        errors += 1;
      }

      struct tm timeinfo = {0};
      char *libc_t = strptime(buf, "%Y%m%d%H%M%S", &timeinfo);
      assert(libc_t);

      EXPECT((int)t.year - 1900, timeinfo.tm_year);
      EXPECT((int)t.month, timeinfo.tm_mon);
      EXPECT((int)t.mday, timeinfo.tm_mday);
      EXPECT((int)t.hour, timeinfo.tm_hour);
      EXPECT((int)t.minute, timeinfo.tm_min);
      EXPECT((int)t.second, timeinfo.tm_sec);

      if (timeinfo.tm_sec == 60) {
        printf("Found leap second %s", buf); // Does not occur. Hmm.
      }
    }
  }

  return errors == 0;
}

int main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;
  _Bool ok = test_all_valid();
  return ok ? 0 : 1;
}

#else // default program

#include <time.h>

int main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  {
    char buf[16] = "19801801000000\0\0";

    struct tm timeinfo = {0};
    char *libc_t = strptime("19800801000000", "%Y%m%d%H%M%S", &timeinfo);
    assert(libc_t);

    struct sse_parsed_time t = sse_parse_time(buf);
  }

  {
    char buf[16] = "19690101000000\0\0";
    struct sse_parsed_time t = sse_parse_time(buf);
    if (t.error) {
      printf("%s\n", t.error);
    }
  }

  return 0;
}
#endif


/* Local Variables: */
/* compile-command: "cc simd_parse_time.c -Wall -Wextra -fsanitize=address,undefined -march=native -fopenmp -ggdb -O0" */
/* End: */
