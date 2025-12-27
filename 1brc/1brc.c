#if IN_SHELL /* $ bash 1brc.c
# cc 1brc.c -o 1brc -fsanitize=undefined -Wall -Wextra -g3 -O0
cc 1brc.c -o 1brc -Wall -Wextra -O3 -march=native -DNDEBUG
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

#include "profiler.h"
#include "helpers.h"

static struct { unsigned char *beg, *end; } input;

static double parse_temperature_naive(S8 s) {
  PROF_FUNCTION_BEGIN;
  _Bool sign = s.data[0] == '-';
  if (sign) { s.data++; s.len--; }

  uintptr_t v = 0;
  while (s.len > 0 && s.data[0] != '.') {
    int n = s.data[0] - '0';
    assert(n >= 0 && n <= 9);
    v *= 10;
    v += n;
    s.data++; s.len--;
  }

  if (s.len > 0) { s.data++; s.len--; } // skip dot

  uintptr_t f = 0;
  uintptr_t f_pow = 1;
  while (s.len > 0) {
    int n = s.data[0] - '0';
    assert(n >= 0 && n <= 9);
    f_pow *= 10;
    f *= 10;
    f += n;
    s.data++; s.len--;
  }

  double result = (double)v + (double)(f)/(double)(f_pow);
  if (sign) result = -result;
  PROF_FUNCTION_END;
  return result;
}

typedef struct CityRecord CityRecord;
struct CityRecord {
  S8 name;
  double min_temp;
  double max_temp;
  double acc_temp;
  uintptr_t hit_count;
  CityRecord *left, *right; // BST links
};

enum { TABLE_COUNT_EXP = 15 };
static struct Measurements {
  CityRecord table[1 << TABLE_COUNT_EXP];
  CityRecord *bst;
} measurements = {0};

static void bst_insert_record(CityRecord *parent, CityRecord *record) {
  if (parent == record) return;
  int cmp = s8cmp(parent->name, record->name);
  if (cmp == 0) {
    return;
  }
  else if (cmp < 0) {
    if (parent->right) bst_insert_record(parent->right, record);
    else               parent->right = record;
  }
  else if (cmp > 0) {
    if (parent->left) bst_insert_record(parent->left, record);
    else              parent->left = record;
  }
}

static void bst_print(CityRecord *record) {
  if (record->left) bst_print(record->left);
  printf("'%.*s'\t%.2f / %.2f / %.2f\n",
         (int)record->name.len, record->name.data,
         record->min_temp,
         (double)record->acc_temp/(double)record->hit_count,
         record->max_temp);
  if (record->right) bst_print(record->right);
}

static void *entry_point_naive(void *arg)
{
  PROF_FUNCTION_BEGIN;
  prof_globals.throughput_data_sz = input.end - input.beg;

  uintptr_t lane_idx = (uintptr_t)arg;
  if (lane_idx != 0) return 0;

  S8 input_s = {input.beg, input.end - input.beg};

  Cut linecutter = (Cut){{}, input_s};
  for (;;) {
    prof_zone_enter("line parse");
    linecutter = cut(linecutter.tail, '\n');
    Cut measurement   = cut(linecutter.head, ';');
    S8  city          = measurement.head;
    S8  temperature_s = measurement.tail;
    prof_zone_exit();
    if (city.len == 0) break;

    double temp       = parse_temperature_naive(temperature_s);

    prof_zone_enter("upsert");
    CityRecord *record = 0;
    { // record = upsert(measurements.table, city)
      uint64_t h = s8hash(city);
      for (uint64_t idx = h;;) {
        idx = hash_table_idx_lookup(h, idx, TABLE_COUNT_EXP);
        if (measurements.table[idx].name.data == 0) {
          measurements.table[idx].name = city;
          measurements.table[idx].min_temp = temp;
          measurements.table[idx].max_temp = temp;
          record = &measurements.table[idx];
          prof_zone_enter("bst insert");
          if (measurements.bst) bst_insert_record(measurements.bst, record);
          else                  measurements.bst = record;
          prof_zone_exit();
          break;
        }
        else if (measurements.table[idx].name.len == city.len) {
          if (s8cmp(measurements.table[idx].name, city) == 0) {
            record = &measurements.table[idx];
            break;
          }
        }
      }
    }
    if (temp < record->min_temp) record->min_temp = temp;
    if (temp > record->max_temp) record->max_temp = temp;
    record->acc_temp += temp;
    record->hit_count++;
    prof_zone_exit();
  }

  prof_zone_enter("bst_print");
  bst_print(measurements.bst);
  prof_zone_exit();

  PROF_FUNCTION_END;
  return 0;
}

int main() {
  int fd = open("measurements.txt", O_RDONLY);
  /* int fd = open("1000.lines", O_RDONLY); */
  if (fd < 0) { perror("openat: "); abort(); }

  struct stat stat;
  int ok = fstat(fd, &stat);
  if (ok < 0) { perror("fstat: "); abort(); }

  input.beg = (unsigned char *)mmap(0, stat.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE | MAP_HUGE_2GB, fd, 0);
  input.end = input.beg + stat.st_size;

  entry_point_naive((void*)0);

  close(fd);
}
