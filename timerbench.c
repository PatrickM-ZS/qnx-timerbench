#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <sys/siginfo.h>
#include <sys/dispatch.h>

#include <sched.h>
#include <pthread.h>

#include "dds/dds.h"

typedef dds_time_t (*dds_time_func_t)(void);
typedef ddsrt_mtime_t (*dds_time_monotonic_func_t)(void);

/* This could move to a ddsrt_time_init() */
static uint64_t hrtime_base_cycles;
static uint64_t hrtime_base_monotonic_ns;
static uint64_t hrtime_base_realtime_ns;
static uint64_t hrtime_cycles_per_sec;

void init_hrtimers(void) {
  const struct qtime_entry *qtime = SYSPAGE_ENTRY(qtime);
  hrtime_cycles_per_sec = qtime->cycles_per_sec;
  
  struct timespec ts_realtime, ts_monotonic;
  clock_gettime(CLOCK_REALTIME, &ts_realtime);
  clock_gettime(CLOCK_MONOTONIC, &ts_monotonic);

  hrtime_base_realtime_ns = ts_realtime.tv_sec * 1000000000ULL + ts_realtime.tv_nsec;
  hrtime_base_monotonic_ns = ts_monotonic.tv_sec * 1000000000ULL + ts_monotonic.tv_nsec;
  hrtime_base_cycles = ClockCycles();
}
/* end ddsrt_time_init() */

static inline uint64_t cycles_to_ns(uint64_t cycles) {
  return (cycles * 1000000000ULL) / hrtime_cycles_per_sec;
}

static inline uint64_t ts_to_ns(const struct timespec* ts) {
  return ((uint64_t)ts->tv_sec * 1000000000ULL) + ts->tv_nsec;
}

dds_time_t dds_hr_time()
{
  uint64_t now = ClockCycles();
  uint64_t delta = now - hrtime_base_cycles;
  return hrtime_base_realtime_ns + cycles_to_ns(delta);
}

ddsrt_mtime_t ddsrt_hr_time_monotonic()
{
  uint64_t now = ClockCycles();
  uint64_t delta = now - hrtime_base_cycles;
  return (ddsrt_mtime_t) { hrtime_base_monotonic_ns + cycles_to_ns(delta) };
}

void dds_hr_sleepfor(dds_duration_t reltime)
{
  if (reltime > 0) {
    timer_t tid;
    struct sigevent sev;
    struct itimerspec its, itst;
    sigset_t mask;
    int sig = SIGRTMIN;

    sigemptyset(&mask);
    sigaddset(&mask, sig);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = sig;

    timer_create(CLOCK_REALTIME, &sev, &tid);

    its.it_value.tv_sec = reltime / 1000000000;
    its.it_value.tv_nsec = reltime % 1000000000;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;

    memset(&itst, 0, sizeof(itst));
    itst.it_value.tv_nsec = 1;
    if (timer_settime(tid, TIMER_TOLERANCE, &itst, NULL) == -1) {
      printf("failed to set timer tolerance\n");
    }

    timer_settime(tid, 0, &its, NULL);

    siginfo_t si;
    sigwaitinfo(&mask, &si);

    timer_delete(tid);
  }
}

#if 0
void dds_hr_sleepfor(dds_duration_t reltime)
{
  if (reltime > 0) {
    timer_t tid;
    struct sigevent sev;
    struct itimerspec its;

    int chid = ChannelCreate(0);
    if (chid == -1) {
      printf("ChannelCreate failed\n");
      return;
    }

    SIGEV_PULSE_INIT(&sev, chid, SIGEV_PULSE_PRIO_INHERIT, 0xab, 0);
    if (timer_create(CLOCK_MONOTONIC, &sev, &tid) == -1) {
      printf("timer_create failed\n");
      ChannelDestroy(0);
      return;
    }

    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = reltime / DDS_NSECS_IN_SEC;
    its.it_value.tv_nsec = reltime % DDS_NSECS_IN_SEC;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;

    if (timer_settime(tid, 0, &its, NULL) == -1) {
      printf("timer_settime failed\n");
      timer_delete(tid);
      ChannelDestroy(chid);
      return;
    }

    struct _pulse pulse;
    printf("waiting for pulse...\n");
    int rc = MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL);
    if (rc < 0) {
      printf("MsgReceivePulse failed\n");
    }
    printf("Pulse received %d\n", pulse.code);

    timer_delete(tid);
    ChannelDestroy(chid);
  }
  return;
}
#endif

long get_clockres(clockid_t id)
{
  struct timespec res;
  if (clock_getres(id, &res) != 0) {
    fprintf(stderr, "Error getting clock resolution: %s\n", strerror(errno));
    return -1;
  }
  return (res.tv_sec * 1000000L + res.tv_nsec / 1000L);
}

void bench_clock_gettime_res(clockid_t id, int rounds, const char *strid)
{
  struct timespec t1, t2;
  uint64_t min_delta = UINT64_MAX;
  int round_at_min = -1;
  int max_spins = 0;
  uint64_t total_spins = 0;
  for (int i = 0; i < rounds; i++) {
    int spins = 0;
    clock_gettime(id, &t1);
    do {
      clock_gettime(id, &t2);
      spins++;
    } while (ts_to_ns(&t2) == ts_to_ns(&t1));

    if (spins > max_spins) {
      max_spins = spins;
    }
    total_spins += spins;
    
    uint64_t delta = ts_to_ns(&t2) - ts_to_ns(&t1);
    if (delta < min_delta) {
      min_delta = delta;
      round_at_min = i + 1;
    }
  }

  double avg_spins = (double)total_spins / rounds;
  printf("  %-30s: %7luns (round: %d, spins: avg=%.2f max=%d total=%u)\n",
    strid, min_delta, round_at_min, avg_spins, max_spins, total_spins);
}

void bench_clockcycles_res(uint64_t cycles_per_sec, int rounds, const char *strid)
{
  uint64_t t1, t2;
  uint64_t min_delta = UINT64_MAX;
  int round_at_min = -1;
  int max_spins = 0;
  uint64_t total_spins = 0;
  for (int i = 0; i < rounds; i++) {
    int spins = 0;
    t1 = ClockCycles();
    do {
      t2 = ClockCycles();
      spins++;
    } while (t1 == t2);

    if (spins > max_spins) {
      max_spins = spins;
    }
    total_spins += spins;

    uint64_t delta = t2 - t1;
    if (delta < min_delta) {
      min_delta = delta;
      round_at_min = i + 1;
    }
  }

  double avg_spins = (double)total_spins / rounds;
  printf("  %-30s: %7lu   (round: %d, spins: avg=%.2f max=%d total=%u)\n",
    strid, min_delta, round_at_min, avg_spins, max_spins, total_spins);
}

void bench_ddstime_res(bool use_hr, int rounds, const char *strid)
{
  dds_time_t t1, t2;
  uint64_t min_delta = UINT64_MAX;
  int round_at_min = -1;
  int max_spins = 0;
  uint64_t total_spins = 0;

  dds_time_func_t dds_time_func = use_hr ? dds_hr_time : dds_time;

  for (int i = 0; i < rounds; i++) {
    int spins = 0;
    t1 = dds_time_func();
    do {
      t2 = dds_time_func();
      spins++;
    } while (t1 == t2);

    if (spins > max_spins) {
      max_spins = spins;
    }
    total_spins += spins;

    uint64_t delta = t2 - t1;
    if (delta < min_delta) {
      min_delta = delta;
      round_at_min = i + 1;
    }
  }

  double avg_spins = (double)total_spins / rounds;
  printf("  %-30s: %7luns (round: %d, spins: avg=%.2f max=%d total=%u)\n",
    strid, min_delta, round_at_min, avg_spins, max_spins, total_spins);
}

void bench_ddstime_monotonic_res(bool use_hr, int rounds, const char *strid)
{
  ddsrt_mtime_t t1, t2;
  uint64_t min_delta = UINT64_MAX;
  int round_at_min = -1;
  int max_spins = 0;
  uint64_t total_spins = 0;

  dds_time_monotonic_func_t dds_time_monotonic_func = use_hr ? ddsrt_hr_time_monotonic : ddsrt_time_monotonic;

  for (int i = 0; i < rounds; i++) {
    int spins = 0;
    t1 = dds_time_monotonic_func();
    do {
      t2 = dds_time_monotonic_func();
      spins++;
    } while (t1.v == t2.v);

    if (spins > max_spins) {
      max_spins = spins;
    }
    total_spins += spins;

    uint64_t delta = t2.v - t1.v;
    if (delta < min_delta) {
      min_delta = delta;
      round_at_min = i + 1;
    }
  }

  double avg_spins = (double)total_spins / rounds;
  printf("  %-30s: %7luns (round: %d, spins: avg=%.2f max=%d total=%u)\n",
    strid, min_delta, round_at_min, avg_spins, max_spins, total_spins);
}

#define bench_clock_perf(clock_expr, rounds, strid) do { \
    uint64_t ctotal = 0;                                 \
    uint64_t cmin = UINT64_MAX;                          \
    uint64_t cmax = 0;                                   \
    uint64_t cdiff;                                      \
    double cavg;                                         \
    for (int i = 0; i < (rounds); i++) {                 \
      uint64_t before = ClockCycles();                   \
      (void)(clock_expr);                                \
      uint64_t after = ClockCycles();                    \
                                                         \
      cdiff = (after - before);                          \
      ctotal += cdiff;                                   \
      if (cdiff > cmax)                                  \
        cmax = cdiff;                                    \
      if (cdiff < cmin)                                  \
        cmin = cdiff;                                    \
    }                                                    \
    cavg = (double)ctotal / rounds;                      \
    printf("  %-30s: min=%-3lu avg=%-3.2f max=%-3lu total=%-3lu cycles\n",\
      (strid), cmin, cavg, cmax, ctotal);                \
  } while (0)

void bench_sleep_prec(void (*sleep_func)(dds_duration_t), const char *strid, int rounds, const uint64_t *intervals, size_t num_intervals)
{
  double ns_per_cycle = (double)1000000000L / SYSPAGE_ENTRY(qtime)->cycles_per_sec;

  printf("%-30s:\n", strid);
  
  for (int i = 0; i < num_intervals; i++) {
    uint64_t sleep_ns = intervals[i];
    uint64_t deltas[rounds];
    uint64_t total = 0;
    uint64_t min = UINT64_MAX;
    uint64_t max = 0;

    for (int j = 0; j < rounds; j++) {
      uint64_t before = ClockCycles();
      //dds_sleepfor(sleep_ns);
      sleep_func((dds_duration_t)sleep_ns);
      uint64_t after = ClockCycles();

      uint64_t delta_ns = (uint64_t)((after - before) * ns_per_cycle);
      deltas[i] = delta_ns;
      total += delta_ns;
      if (delta_ns > max)
        max = delta_ns;
      if (delta_ns < min)
        min = delta_ns;
    }

    double avg = total / rounds;
    double var = 0.0;
    for (int j = 0; j < rounds; j++) {
      double diff = deltas[i] - avg;
      var += diff * diff;
    }
    double dev = sqrt(var / rounds);
    printf("%30lu: %13luns %13luns %13.1fns %13.1fns\n", sleep_ns, min, max, avg, dev);
  }
} 

void pin_to_core(int id) {
  uintptr_t mask = 1UL << id;
  if (ThreadCtl(_NTO_TCTL_RUNMASK, (void*)mask) == -1) {
    printf("ThreadCtl failed\n");
  } else {
    printf("Thread pinned to core %d\n", id);
  }
}

int main(int argc, char **argv)
{
  struct timespec ts;
  long res;

  //pin_to_core(0);
  init_hrtimers();

  printf("System clock resolution:\n");
  if ((res = get_clockres(CLOCK_REALTIME)) > 0) {
    printf("  CLOCK_REALTIME: %ldus\n", res);
  }
  if ((res = get_clockres(CLOCK_MONOTONIC)) > 0) {
    printf("  CLOCK_MONOTONIC: %ldus\n", res);
  }

  const struct qtime_entry *qtime = SYSPAGE_ENTRY(qtime);
  time_t sec = qtime->boot_time / 1000000000ULL;
  struct tm *tm = gmtime(&sec);
  printf("Clock cycles per sec: %u\n", qtime->cycles_per_sec);
  printf("Nanoseconds per cycle: %d\n", 1000000000L / qtime->cycles_per_sec);
  printf("Boot time (ns since epoch): %u\n", qtime->boot_time);
  printf("Boot time (UTC): %s", asctime(tm));
  printf("\n");

  // Minimum delta between two calls returning a different timestamp
  int rounds = 100;
  printf("Benchmark (minimum) clock resolution (%d rounds):\n", rounds);
  bench_clock_gettime_res(CLOCK_REALTIME, rounds, "clock_gettime(CLOCK_REALTIME)");
  bench_clock_gettime_res(CLOCK_MONOTONIC, rounds, "clock_gettime(CLOCK_MONOTONIC)");
  bench_clockcycles_res(qtime->cycles_per_sec, rounds, "ClockCycles()");
  bench_ddstime_res(false, rounds, "dds_time()");
  bench_ddstime_monotonic_res(false, rounds, "ddsrt_time_monotonic()");
  bench_ddstime_res(true, rounds, "dds_hr_time()");
  bench_ddstime_monotonic_res(true, rounds, "dds_hr_time_monotonic()");
  printf("\n");

  // Execution time of clock calls
  rounds = 1000;
  printf("Benchmark clock query performance (%d rounds):\n", rounds);
  bench_clock_perf(clock_gettime(CLOCK_REALTIME, &(struct timespec){0}), rounds, "clock_gettime(CLOCK_REALTIME)");
  bench_clock_perf(clock_gettime(CLOCK_MONOTONIC, &(struct timespec){0}), rounds, "clock_gettime(CLOCK_MONOTONIC)");
  bench_clock_perf(ClockCycles(), rounds, "ClockCycles()");
  bench_clock_perf(dds_time(), rounds, "dds_time()");
  bench_clock_perf(ddsrt_time_monotonic(), rounds, "ddsrt_time_monotonic()");
  bench_clock_perf(dds_hr_time(), rounds, "dds_time_hr()");
  bench_clock_perf(ddsrt_hr_time_monotonic(), rounds, "ddsrt_hr_time_monotonic()");
  printf("\n");

  // nanosleep precision
  rounds = 10;
  printf("Benchmark sleep precision (%d rounds):\n", rounds);
  const uint64_t intervals[] = { 1, 100, 1000, 1000000, 2000000, 3000000, 10000000 };
  const size_t num_intervals = sizeof(intervals) / sizeof(intervals[0]);
  printf("%-30s %-15s %-15s %-15s %-15s\n", "function\trequested (ns)", "(elapsed) min", "max", "avg", "stddev");
  bench_sleep_prec(dds_sleepfor, "dds_sleepfor()", rounds, intervals, num_intervals);
  bench_sleep_prec(dds_hr_sleepfor, "dds_hr_sleepfor()", rounds, intervals, num_intervals);
}

