#if IN_SHELL /* $ bash 1brc_multicore.c
 # perf:
 cc 1brc_multicore.c -o 1brc_multicore -Wall -Wextra -O3 -march=native -pthread -DNDEBUG -DNPROFILER
 # perf + profiler:
 # cc 1brc_multicore.c -o 1brc_multicore -Wall -Wextra -O3 -march=native -pthread
 # debug:
 # cc 1brc_multicore.c -o 1brc_multicore -Wall -Wextra -O0 -ggdb -fsanitize=undefined,thread -march=native -pthread
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
#include "sys/stat.h"

#include <pthread.h>

#include <immintrin.h>

#define NUM_LANES 12

static __thread struct {
  uintptr_t lane_idx;
} tctx = {0};

#include "profiler.h"
#include "helpers.h"

typedef struct CityRecord CityRecord;
struct CityRecord {
  uint64_t name_hash;
  int16_t min_temp, max_temp;
  int32_t acc_temp, hit_count;
  S8 name; // View into mm->city_name_storage
};

typedef struct Measurements Measurements;
struct Measurements {
  #define MAX_UNIQUE_CITY_NAMES (1 << 14)
  CityRecord results[MAX_UNIQUE_CITY_NAMES];
  intptr_t results_count;

  #define HASH_TABLE_COUNT_EXP (16)
  CityRecord *hash_table[1 << HASH_TABLE_COUNT_EXP];

  #define CITY_NAME_STORAGE_CAPACITY (128 * MAX_UNIQUE_CITY_NAMES)
  unsigned char city_name_storage[CITY_NAME_STORAGE_CAPACITY];
  intptr_t      city_name_storage_count;
};

// Shared storage
static struct {
  int fd;
  size_t fd_sz;
  pthread_barrier_t barrier;
} globals;

// Per-thread storage
static Measurements measurements[NUM_LANES];

static S8 dup_city_name(Measurements *mm, S8 city) {
  assert((mm->city_name_storage_count + city.len) < CITY_NAME_STORAGE_CAPACITY);
  S8 result = { .data = mm->city_name_storage + mm->city_name_storage_count, .len = city.len };
  memcpy(result.data, city.data, city.len);
  mm->city_name_storage_count += city.len;
  return result;
}

static int city_record_cmp(const void *a, const void *b) {
  CityRecord *ra = (CityRecord *)a;
  CityRecord *rb = (CityRecord *)b;
  return s8cmp(ra->name, rb->name);
}

unsigned char *process_chunk(Measurements *mm, unsigned char *batch_beg, unsigned char *batch_end) {
  PROF_FUNCTION_BEGIN;

  for (; batch_beg < batch_end && *batch_beg;) {

    enum { NUM_BATCHES = 256 };
    struct { S8 city_s, temp_s; } batches[NUM_BATCHES];
    uint32_t batches_count = 0;

    PROFILE_BLOCK("batch_line_parse") {
      unsigned char* cur_line_beg = batch_beg;
      unsigned char* semicolon_ptr = 0;
      for (unsigned char* at = batch_beg; at < batch_end && batches_count < NUM_BATCHES && *at; at += 32) {
        __m256i chars      = _mm256_loadu_si256((__m256i*)at);
        __m256i semicolons = _mm256_cmpeq_epi8(chars, _mm256_set1_epi8(';'));
        __m256i newlines   = _mm256_cmpeq_epi8(chars, _mm256_set1_epi8('\n'));

        uint32_t semicolon_mask = _mm256_movemask_epi8(semicolons);
        uint32_t newline_mask   = _mm256_movemask_epi8(newlines);

        if (semicolon_ptr == 0 && semicolon_mask) {
          semicolon_ptr = at + __builtin_ctz(semicolon_mask);
        }

        while (newline_mask && batches_count < NUM_BATCHES) {
          uint32_t newline_idx = __builtin_ctz(newline_mask);
          newline_mask &= ~(1u << newline_idx);

          unsigned char *newline_ptr = at + newline_idx;

          _Bool have_key_value_pair = semicolon_ptr && semicolon_ptr > cur_line_beg && semicolon_ptr < newline_ptr;
          if (have_key_value_pair) {
            unsigned char *last = newline_ptr + 1;
            if (last < batch_end) {
              batches[batches_count].city_s = (S8) { cur_line_beg, semicolon_ptr - cur_line_beg };
              batches[batches_count].temp_s = (S8) { semicolon_ptr + 1, newline_ptr - semicolon_ptr - 1 };
              batches_count++;
            }
          }

          // Advance to next line
          cur_line_beg = newline_ptr + 1;
          semicolon_ptr = 0;

          // Store remaining semicolon for future  (part of next line)
          if (newline_idx < 31) {
            uint32_t remaining_semi = semicolon_mask & ~((1u << (newline_idx + 1)) - 1);
            if (remaining_semi) {
              int next_semi = __builtin_ctz(remaining_semi);
              semicolon_ptr = at + next_semi;
            }
          }
        }
      }
    }

    if (batches_count == 0) { break; }

    for (uint32_t i = 0; i < batches_count; i++) {

      S8 city_s = batches[i].city_s;
      S8 temp_s = batches[i].temp_s;
      int16_t temp;

      PROFILE_BLOCK("parse_temp") {
        uint8_t *d = temp_s.data;
        int is_neg = (temp_s.data[0] == '-');
        int s = is_neg; // start index
        temp = (temp_s.len == 3 + is_neg) ?
          (d[s] - '0') * 10  + (d[s+2] - '0') :                       //  X.Y or  -X.Y
          (d[s] - '0') * 100 + (d[s+1] - '0') * 10 + (d[s+3] - '0');  // XX.Y or -XX.Y
        temp = is_neg ? -temp : temp;
      }

      PROFILE_BLOCK("upsert") {
        uint64_t h = s8hash(city_s);
        for (uint64_t idx = h;;) {
          idx = hash_table_idx_lookup(h, idx, HASH_TABLE_COUNT_EXP);
          CityRecord *candidate = mm->hash_table[idx];
          if (candidate == 0) {
            candidate = &mm->results[mm->results_count++];
            mm->hash_table[idx] = candidate;
            candidate->name = dup_city_name(mm, city_s);
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
    }
    batch_beg = batches[batches_count - 1].temp_s.data + batches[batches_count - 1].temp_s.len + 1;
  }

  PROF_FUNCTION_END;
  return batch_beg;
}

static void *entry_point(void *arg) {
  PROF_FUNCTION_BEGIN;

  tctx.lane_idx = (uintptr_t)arg;

  Measurements *mm = &measurements[tctx.lane_idx];
  mm->results_count = 0;
  mm->city_name_storage_count = 0;

  enum {
    READ_BUFFER_SZ  = 1u << 16, // 64KiB chunks
    MAX_LINE_LENGHT = 128,      // Line margin
  };

  unsigned char read_buffer[READ_BUFFER_SZ + 32]; // +32 for SIMD overread
  intptr_t      read_buffer_valid_bytes = 0;
  intptr_t      read_buffer_start = 0;

  memset(read_buffer + READ_BUFFER_SZ, 0, 32); // overread should be zero

  // Calculate this thread's file region
  uintptr_t per_thread_sz = globals.fd_sz / NUM_LANES;
  intptr_t file_offset = (tctx.lane_idx > 0) ?
    per_thread_sz * tctx.lane_idx - MAX_LINE_LENGHT : 0;
  intptr_t file_end = (tctx.lane_idx == NUM_LANES - 1) ?
    globals.fd_sz : per_thread_sz * (tctx.lane_idx + 1);

  { // Read first chunk and find line boundary
    ssize_t to_read = min(READ_BUFFER_SZ, file_end - file_offset);
    ssize_t bytes_read = pread(globals.fd, read_buffer, to_read, file_offset);
    if (bytes_read < 0) { perror("pread"); abort(); }
    file_offset += bytes_read;
    read_buffer_valid_bytes = bytes_read;

    // Back up to start of line
    if (tctx.lane_idx > 0) {
      read_buffer_start = MAX_LINE_LENGHT;
      while (read_buffer_start > 0 && read_buffer[read_buffer_start] != '\n') {
        read_buffer_start--;
      }
      if (read_buffer[read_buffer_start] == '\n') read_buffer_start++;
    }
  }

  while (file_offset < file_end) {
    unsigned char *processed_end =
        process_chunk(mm, read_buffer + read_buffer_start,
                          read_buffer + read_buffer_valid_bytes);

    intptr_t processed_bytes = processed_end - (read_buffer + read_buffer_start);
    if (processed_bytes == 0) break;

    // Calculate remaining unprocessed data
    intptr_t remaining = read_buffer_valid_bytes - (processed_end - read_buffer);
    if (remaining > 0) {
      memmove(read_buffer, processed_end, remaining);
    }

    intptr_t to_read = min(READ_BUFFER_SZ - remaining, file_end - file_offset);
    if (to_read > 0) {
      intptr_t new_bytes = pread(globals.fd, read_buffer + remaining, to_read, file_offset);
      if (new_bytes <  0) { perror("pread"); abort(); }
      if (new_bytes == 0) { break; }
      file_offset += new_bytes;
      if (file_offset >= file_end) break;
      read_buffer_valid_bytes = remaining + new_bytes;
    }
    else {
      read_buffer_valid_bytes = remaining;
    }
    read_buffer_start = 0; // Newline on subsequent chunks should be guaranteed by process_chunk()
  }

  // Sync and join results for main thread
  pthread_barrier_wait(&globals.barrier);

  if (tctx.lane_idx == 0) {
    for (intptr_t other_lane_idx = 1; other_lane_idx < NUM_LANES; other_lane_idx++) {
      for (intptr_t other_record_idx = 0; other_record_idx < measurements[other_lane_idx].results_count; other_record_idx++) {
        CityRecord *other_record = &measurements[other_lane_idx].results[other_record_idx];
        for (uint64_t idx = other_record->name_hash;;) {
          idx = hash_table_idx_lookup(other_record->name_hash, idx, HASH_TABLE_COUNT_EXP);
          CityRecord *candidate = mm->hash_table[idx];
          if (candidate == 0) {
            candidate = &mm->results[mm->results_count++];
            mm->hash_table[idx] = candidate;
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

    qsort(mm->results, mm->results_count, sizeof(mm->results[0]), city_record_cmp);

    for (intptr_t i = 0; i < mm->results_count; i++) {
      CityRecord *record = &mm->results[i];
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

#include <sys/time.h>
double get_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

int main(int argc, char **argv) {
  PROF_FUNCTION_BEGIN;

  (void)argv;

  int fd = open("measurements.txt", O_RDONLY);
  if (fd < 0) { perror("open"); abort(); }

  int ok;
  struct stat stat;
  ok = fstat(fd, &stat);
  if (ok < 0) { perror("fstat"); abort(); }

  globals.fd = fd;
  globals.fd_sz = stat.st_size;
  prof_globals.throughput_data_sz = globals.fd_sz;

  ok = pthread_barrier_init(&globals.barrier, 0, NUM_LANES);
  if (ok < 0) { perror("pthread_barrier_init"); abort(); }

  double start_time = get_time();

  pthread_t threads[NUM_LANES];
  for (intptr_t i = 1; i < NUM_LANES; i++) {
    ok = pthread_create(&threads[i], 0, entry_point, (void *)i);
    if (ok < 0) { perror("pthread_create"); abort(); }
  }
  entry_point((void*)0);
  for (int i = 1; i < NUM_LANES; i++) {
    ok = pthread_join(threads[i], 0);
    if (ok < 0) { perror("pthread_join"); abort(); }
  }

  if (argc > 1) {
    double elapsed = get_time() - start_time;
    fprintf(stderr, "\nResults:\n");
    fprintf(stderr, "Threads: %d\n", NUM_LANES);
    fprintf(stderr, "File size: %.2f MB\n", (double)globals.fd_sz / (1024 * 1024));
    fprintf(stderr, "Time: %.3f seconds\n", elapsed);
    fprintf(stderr, "Throughput: %.2f GB/s\n", (globals.fd_sz / (1024.0 * 1024.0 * 1024.0)) / elapsed);
  }

  close(fd);
  PROF_FUNCTION_END;
  return 0;
}
