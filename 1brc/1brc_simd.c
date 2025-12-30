#if IN_SHELL /* $ bash 1brc_simd.c
 # cc 1brc_simd.c -o 1brc_simd -fsanitize=undefined -Wall -Wextra -g3 -O0 -march=native
   cc 1brc_simd.c -o 1brc_simd -Wall -Wextra -O3 -march=native # -DNDEBUG
exit # */
#endif

#include "stdint.h"
#include "stddef.h"
#include "assert.h"

#include "stdio.h"
#include "stdlib.h"
#include "string.h"

#include "fcntl.h"
#include "unistd.h"
#include "sys/mman.h"
#include "sys/stat.h"

#include <immintrin.h>

static __thread struct {
  uintptr_t lane_idx;
} tctx = {0};

#include "profiler.h"
#include "helpers.h"

static struct { unsigned char *beg, *end; } input;

typedef struct CityRecord CityRecord;
struct CityRecord {
  S8 name;
  int32_t min_temp;
  int32_t max_temp;
  int64_t acc_temp;
  uintptr_t hit_count;
};

enum { TABLE_COUNT_EXP = 15 };
static struct Measurements {
  CityRecord table[1 << TABLE_COUNT_EXP];
  CityRecord *results[1 << 14];
  intptr_t results_count;
} measurements = {0};


static int city_record_cmp(const void *a, const void *b) {
  CityRecord *ra = *(CityRecord **)a;
  CityRecord *rb = *(CityRecord **)b;
  return s8cmp(ra->name, rb->name);
}

static void *entry_point_simd(void *arg) {
  PROF_FUNCTION_BEGIN;
  prof_globals.throughput_data_sz = input.end - input.beg;

  uintptr_t lane_idx = (uintptr_t)arg;
  if (lane_idx != 0) return 0;

  assert(((input.end - input.beg) % 32) == 0);

  unsigned char *line_beg = input.beg;
  while (line_beg < input.end && *line_beg != 0) {

    struct {
      uint32_t newline_idx;
      S8 city, integer, fraction;
    } parsed = {0};

    PROFILE_BLOCK("line_parse") {
      __m256i chars = (__m256i) (*(__v32qi_u *) line_beg);
      __m256i match_newline = _mm256_cmpeq_epi8(chars, _mm256_set1_epi8('\n'));
      uint32_t newline_idx = __builtin_ctz(_mm256_movemask_epi8(match_newline));
      assert(line_beg[newline_idx] == '\n');

      __m256i match_semicolon = _mm256_cmpeq_epi8(chars, _mm256_set1_epi8(';'));
      uint32_t semicolon_idx = __builtin_ctz(_mm256_movemask_epi8(match_semicolon));

      __m256i match_dot = _mm256_cmpeq_epi8(chars, _mm256_set1_epi8('.'));
      uint32_t dot_mask = _mm256_movemask_epi8(match_dot);
      dot_mask &= ~((1u << semicolon_idx) - 1); // Exclude dot match in city name e.g. 'Washington D.C.'
      uint32_t dot_idx = __builtin_ctz(dot_mask);

      assert(semicolon_idx < newline_idx);
      assert(dot_idx > semicolon_idx);
      assert(dot_idx < newline_idx);

      parsed.newline_idx = newline_idx;
      parsed.city     = (S8){ line_beg + 0, semicolon_idx };
      parsed.integer  = (S8){ line_beg + semicolon_idx + 1, dot_idx - semicolon_idx - 1 };
      parsed.fraction = (S8){ line_beg + dot_idx + 1, newline_idx - dot_idx - 1 };
    }

    int32_t temperature = 0;
    PROFILE_BLOCK("parse_temperature") {
      intptr_t integer = 0;
      {
        unsigned char *at = parsed.integer.data;
        _Bool is_neg = (*at == '-');
        if (is_neg) at++;

        intptr_t value = 0;
        switch (parsed.integer.len - is_neg) {
        case 3: value += ((*at++) - '0')*100; // fallthrough
        case 2: value += ((*at++) - '0')*10;  // fallthrough
        case 1: value += ((*at++) - '0');
        }
        integer = is_neg ? -value : value;
      }
      intptr_t fraction = *parsed.fraction.data - '0';
      temperature = integer * 10 + fraction;
    }

    PROFILE_BLOCK("upsert") {
      CityRecord *record = 0;
      { // record = upsert(measurements.table, city)
        uint64_t h = s8hash(parsed.city);
        for (uint64_t idx = h;;) {
          idx = hash_table_idx_lookup(h, idx, TABLE_COUNT_EXP);
          if (measurements.table[idx].name.data == 0) {
            record = &measurements.table[idx];
            record->name = parsed.city;
            record->min_temp = temperature;
            record->max_temp = temperature;
            measurements.results[measurements.results_count++] = record;
            break;
          }
          else if (s8eq(measurements.table[idx].name, parsed.city)) {
            record = &measurements.table[idx];
            break;
          }
        }
      }
      if (temperature < record->min_temp) record->min_temp = temperature;
      if (temperature > record->max_temp) record->max_temp = temperature;
      record->acc_temp += temperature;
      record->hit_count++;
    }

    line_beg += parsed.newline_idx + 1;
  }

  qsort(measurements.results, measurements.results_count, sizeof(CityRecord*), city_record_cmp);
  for (intptr_t i = 0; i < measurements.results_count; i++) {
    CityRecord *record = measurements.results[i];
    double min = (double)record->min_temp / 10.0;
    double avg = (double)record->acc_temp/(double)record->hit_count / 10.;
    double max = (double)record->max_temp / 10.0;
    printf("%-18.*s %5.1f / %5.1f / %5.1f\n",
           (int)record->name.len, record->name.data,
           min, avg, max);
  }

  PROF_FUNCTION_END;
  return 0;
}

int main() {
  int fd = open("measurements.txt", O_RDONLY);
  if (fd < 0) { perror("open"); abort(); }

  struct stat stat;
  int ok = fstat(fd, &stat);
  if (ok < 0) { perror("fstat"); abort(); }

  uintptr_t align_32_padding = -(uintptr_t)stat.st_size & (32 - 1);
  size_t aligned_size = stat.st_size + align_32_padding;
  input.beg = (unsigned char *)mmap(0, aligned_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
  input.end = input.beg + aligned_size;
  if (input.beg == MAP_FAILED) {
    perror("mmap");
    abort();
  }

  entry_point_simd((void*)0);

  close(fd);
}
