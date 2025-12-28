#include <time.h>

typedef struct Profile_Zone Profile_Zone;
struct Profile_Zone {
  const char *name;
  intptr_t start_time;
  intptr_t hit_count;
  intptr_t child_time;
  intptr_t inclusive_time;
};

static __thread struct {
  Profile_Zone zones[1 << 5];
  intptr_t zones_count;
  Profile_Zone *zone_stack[1 << 10];
  intptr_t zone_stack_count;
  intptr_t throughput_data_sz;
} prof_globals = {0};

#ifdef NPROFILER
#define prof_zone_enter(name)
#define prof_zone_exit()
#else
#define prof_zone_enter(name) prof_zone_enter_(name)
#define prof_zone_exit()      prof_zone_exit_()
#endif

#define DEFER_LOOP(begin, end)        for(int _i_ = ((begin), 0); !_i_; _i_ += 1, (end))
#define PROF_FUNCTION_BEGIN prof_zone_enter(__func__)
#define PROF_FUNCTION_END   prof_zone_exit()
#define PROFILE_BLOCK(name) DEFER_LOOP(prof_zone_enter(name), prof_zone_exit())

static intptr_t prof_rdtscp(void);
static Profile_Zone *prof_get_zone(const char *name);
static void prof_zone_enter_(const char *name);
static void prof_zone_exit_();
static double prof_estimate_cpu_freq(intptr_t time_to_measure_ns);

static intptr_t prof_rdtscp(void) {
  uintptr_t hi, lo;
  asm volatile ("rdtscp" : "=d"(hi), "=a"(lo) :: "cx", "memory");
  return (intptr_t)hi<<32 | lo;
}

static Profile_Zone *prof_get_zone(const char *name) {
  for (intptr_t zone_idx = 0; zone_idx < prof_globals.zones_count; zone_idx++) {
    Profile_Zone *zone = &prof_globals.zones[zone_idx];
    if (strcmp(zone->name, name) == 0) return zone;
  }
  Profile_Zone *zone = &prof_globals.zones[prof_globals.zones_count++];
  zone->name = name;
  return zone;
}

static void __attribute__((unused)) prof_zone_enter_(const char *name) {
  Profile_Zone* zone = prof_get_zone(name);
  zone->start_time = prof_rdtscp();
  prof_globals.zone_stack[prof_globals.zone_stack_count++] = zone;
}

static double prof_estimate_cpu_freq(intptr_t time_to_measure_ns) {
  enum { SECONDS_TO_NS = 1000000000 };
  struct timespec time;
  intptr_t start_tsc = prof_rdtscp();

  clock_gettime(CLOCK_MONOTONIC, &time);
  intptr_t start_ns = time.tv_sec * SECONDS_TO_NS + time.tv_nsec;

  intptr_t current_ns = 0;
  do {
    clock_gettime(CLOCK_MONOTONIC, &time);
    current_ns = time.tv_sec * SECONDS_TO_NS + time.tv_nsec;
  } while (current_ns - start_ns < time_to_measure_ns);

  intptr_t end_tsc = prof_rdtscp();
  intptr_t elapsed_tsc = end_tsc - start_tsc;
  intptr_t elapsed_ns = current_ns - start_ns;
  double ghz = (double)elapsed_tsc / (double)elapsed_ns;
  return ghz * 1e9;
}

static void __attribute__((unused)) prof_zone_exit_() {
  assert(prof_globals.zone_stack_count >= 1);
  Profile_Zone* zone   = prof_globals.zone_stack[prof_globals.zone_stack_count - 1];
  intptr_t elapsed_time = prof_rdtscp() - zone->start_time;
  zone->hit_count++;
  zone->inclusive_time += elapsed_time;
  if (prof_globals.zone_stack_count > 1) {
    Profile_Zone* parent = prof_globals.zone_stack[prof_globals.zone_stack_count - 2];
    parent->child_time += elapsed_time;
  }
  else /* When we pop root node, print report */ {
    printf("\nPROFILER REPORT\n");
    printf("===============\n");
    double cpu_hz = prof_estimate_cpu_freq(100000000 /* 100ms */);
    double total_s = (zone->inclusive_time / cpu_hz);
    double total_us = total_s * 1e6;
    printf("total_s: %.4f s\n", total_s);
    if (prof_globals.throughput_data_sz) {
      double gb = (double)prof_globals.throughput_data_sz / (double)(1024 * 1024 * 1024);
      printf("Data: %.4f GB\n", gb);
      printf("throughtput: %.4f GB/s\n", gb/total_s);
    }

    for (intptr_t zone_idx = 0; zone_idx < prof_globals.zones_count; zone_idx++) {
      Profile_Zone *zone = &prof_globals.zones[zone_idx];
      double inclusive_time = (double)zone->inclusive_time/cpu_hz * 1e6;
      double exclusive_time = (double)((zone->inclusive_time - zone->child_time) / cpu_hz) * 1e6;
      printf("%24s: %9.4fus (%5.1f%%)\t %9.4fus (%5.1f%%) \n",
             zone->name,
             inclusive_time, (inclusive_time / total_us) * 100.0,
             exclusive_time, (exclusive_time / total_us) * 100.0);
    }
  }
  prof_globals.zone_stack_count -= 1;
}
