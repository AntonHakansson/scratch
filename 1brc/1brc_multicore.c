#if IN_SHELL /* $ bash 1brc_multicore.c
 cc 1brc_multicore.c -o 1brc_multicore -Wall -Wextra -O3 -march=native -pthread -DNDEBUG # -DNPROFILER
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

#include <pthread.h>

#include <immintrin.h>

static __thread struct {
  uintptr_t lane_idx;
} tctx = {0};

#include "profiler.h"
#include "helpers.h"

#define NUM_LANES 12
static struct { unsigned char *beg, *end; } global_input;
static pthread_barrier_t barrier;

typedef struct CityRecord CityRecord;
struct CityRecord {
  S8 name;
  uint64_t name_hash;
  int32_t min_temp;
  int32_t max_temp;
  int64_t acc_temp;
  int64_t hit_count;
};

enum { TABLE_COUNT_EXP = 15 };
static struct Measurements {
  CityRecord table[1 << TABLE_COUNT_EXP];
  CityRecord *results[1 << 14];
  intptr_t results_count;
} measurements[NUM_LANES] = {0};

static int city_record_cmp(const void *a, const void *b) {
  CityRecord *ra = *(CityRecord **)a;
  CityRecord *rb = *(CityRecord **)b;
  return s8cmp(ra->name, rb->name);
}

static void *entry_point(void *arg) {
  PROF_FUNCTION_BEGIN;
  prof_globals.throughput_data_sz = global_input.end - global_input.beg;

  tctx.lane_idx = (uintptr_t)arg;
  assert(((global_input.end - global_input.beg) % 32) == 0);

  //- Split work across lanes
  uintptr_t per_thread_sz = (global_input.end - global_input.beg) / NUM_LANES;
  unsigned char *input_beg = global_input.beg + per_thread_sz * (tctx.lane_idx + 0);
  unsigned char *input_end = global_input.beg + per_thread_sz * (tctx.lane_idx + 1);

  //-- Adjust input_beg to start of line
  if (tctx.lane_idx != 0) {
    for (; *input_beg != '\n'; input_beg--) {}
    input_beg++; // Move past the \n to start of next line
  }

  //-- Adjust input_end to end of line
  if (tctx.lane_idx == (NUM_LANES - 1)) { input_end = global_input.end; }
  else {
    for (; *input_end != '\n'; input_end--) {}
  }

  for (unsigned char *line_beg = input_beg; line_beg < input_end && *line_beg;) {

    uint32_t newline_idx = 0;
    S8 city_s, temp_s;
    int32_t temp;

    PROFILE_BLOCK("line_parse") {
      __m256i chars = (__m256i) (*(__v32qi_u *) line_beg);
      __m256i match_newline = _mm256_cmpeq_epi8(chars, _mm256_set1_epi8('\n'));
      newline_idx = __builtin_ctz(_mm256_movemask_epi8(match_newline));
      assert(line_beg[newline_idx] == '\n');

      __m256i match_semicolon = _mm256_cmpeq_epi8(chars, _mm256_set1_epi8(';'));
      uint32_t semicolon_idx = __builtin_ctz(_mm256_movemask_epi8(match_semicolon));
      assert(semicolon_idx < newline_idx);

      city_s  = (S8){ line_beg, semicolon_idx };
      temp_s  = (S8){ line_beg + semicolon_idx + 1, newline_idx - semicolon_idx - 1 };
    }

    PROFILE_BLOCK("parse_temp") {
      __m128i temp_chars = _mm_loadl_epi64((__m128i*)temp_s.data);
      __m128i ascii_zero = _mm_set1_epi8('0');
      __m128i digits = _mm_sub_epi8(temp_chars, ascii_zero);
      uint64_t digit_pack = _mm_cvtsi128_si64(digits);
      uint8_t *d = (uint8_t*)&digit_pack;

      int is_neg = (temp_s.data[0] == '-');
      int s = is_neg; // start index

      temp = (temp_s.len == 3 + is_neg) ?
        d[s] * 10 + d[s+2] :                //  X.Y or  -X.Y
        d[s] * 100 + d[s+1] * 10 + d[s+3];  // XX.Y or -XX.Y

      temp = is_neg ? -temp : temp;
    }

    PROFILE_BLOCK("upsert") {
      uint64_t h = s8hash(city_s);
      for (uint64_t idx = h;;) {
        idx = hash_table_idx_lookup(h, idx, TABLE_COUNT_EXP);
        CityRecord *candidate = &measurements[tctx.lane_idx].table[idx];
        if (candidate->name.data == 0) {
          measurements[tctx.lane_idx].results[measurements[tctx.lane_idx].results_count++] = candidate;
          candidate->name = city_s;
          candidate->name_hash = h;
          candidate->min_temp = candidate->max_temp = candidate->acc_temp = temp;
          candidate->hit_count = 1;
          break;
        }
        else if (candidate->name_hash == h && s8eq(candidate->name, city_s)) {
          if (temp < candidate->min_temp) candidate->min_temp = temp;
          if (temp > candidate->max_temp) candidate->max_temp = temp;
          candidate->acc_temp += temp;
          candidate->hit_count++;
          break;
        }
      }
    }

    line_beg += newline_idx + 1;
  }

  // Sort lexicographically
  qsort(measurements[tctx.lane_idx].results, measurements[tctx.lane_idx].results_count, sizeof(CityRecord*), city_record_cmp);

  pthread_barrier_wait(&barrier);

  if (tctx.lane_idx == 0) {
    for (intptr_t other_lane_idx = 1; other_lane_idx < NUM_LANES; other_lane_idx++) {
      for (intptr_t other_record_idx = 0; other_record_idx < measurements[other_lane_idx].results_count; other_record_idx++) {
        CityRecord *other_record = measurements[other_lane_idx].results[other_record_idx];
        for (uint64_t idx = other_record->name_hash;;) {
          idx = hash_table_idx_lookup(other_record->name_hash, idx, TABLE_COUNT_EXP);
          CityRecord *candidate = &measurements[tctx.lane_idx].table[idx];
          if (candidate->name.data == 0) {
            measurements[tctx.lane_idx].results[measurements[tctx.lane_idx].results_count++] = candidate;
            *candidate = *other_record;
            break;
          }
          else if (candidate->name_hash == other_record->name_hash && s8eq(candidate->name, other_record->name)) {
            if (other_record->min_temp < candidate->min_temp) candidate->min_temp = other_record->min_temp;
            if (other_record->max_temp > candidate->max_temp) candidate->max_temp = other_record->max_temp;
            candidate->acc_temp += other_record->acc_temp;
            candidate->hit_count += other_record->hit_count;
            break;
          }
        }
      }
    }

    qsort(measurements[tctx.lane_idx].results, measurements[tctx.lane_idx].results_count, sizeof(CityRecord*), city_record_cmp);
    for (intptr_t i = 0; i < measurements[tctx.lane_idx].results_count; i++) {
      CityRecord *record = measurements[tctx.lane_idx].results[i];
      double min = (double)record->min_temp / 10.0;
      double avg = (double)record->acc_temp/(double)record->hit_count / 10.;
      double max = (double)record->max_temp / 10.0;
      printf("%-18.*s %5.1f / %5.1f / %5.1f\n",
             (int)record->name.len, record->name.data,
             min, avg, max);
    }
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
  global_input.beg = (unsigned char *)mmap(0, aligned_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
  global_input.end = global_input.beg + aligned_size;
  if (global_input.beg == MAP_FAILED) {
    perror("mmap");
    abort();
  }

  pthread_barrier_init(&barrier, 0, NUM_LANES);

  pthread_t threads[NUM_LANES];
  for (intptr_t i = 1; i < NUM_LANES; i++) {
    int ok = pthread_create(&threads[i], 0, entry_point, (void *)i);
    if (ok < 0) { perror("pthread_create"); abort(); }
  }
  entry_point((void*)0);
  for (int i = 1; i < NUM_LANES; i++) {
    int ok = pthread_join(threads[i], 0);
    if (ok < 0) { perror("pthread_join"); abort(); }
  }

  close(fd);
}
