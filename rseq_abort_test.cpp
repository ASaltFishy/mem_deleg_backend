#include "delegation_backend.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <thread>
#include <unistd.h>

static void signalHandler(int) {}
static void affinity(pthread_t Thread, int Cpu) {
  cpu_set_t Set; CPU_ZERO(&Set); CPU_SET(Cpu, &Set);
  assert(pthread_setaffinity_np(Thread, sizeof(Set), &Set) == 0);
}
int main(int Argc, char **Argv) {
  assert(Argc == 3);
  int Cpu0 = atoi(Argv[1]), Cpu1 = atoi(Argv[2]);
  affinity(pthread_self(), Cpu0);
  struct sigaction Action{}; Action.sa_handler = signalHandler;
  sigemptyset(&Action.sa_mask); assert(sigaction(SIGUSR1, &Action, nullptr) == 0);
  char Domain[96]; snprintf(Domain, sizeof(Domain), "md_rseq_abort_%d", getpid());
  md_config Config{Domain, 0x310000000000ULL, 64ULL << 20, 0};
  md_backend *Backend; assert(md_backend_open(&Backend, &Config) == 0);
  std::atomic<bool> Done{false}; pthread_t Target = pthread_self();
  std::thread Interrupt([&] {
    affinity(pthread_self(), Cpu1);
    for (unsigned I = 0; !Done.load(); ++I) {
      if (I % 4 == 0) affinity(Target, I % 8 == 0 ? Cpu0 : Cpu1);
      assert(pthread_kill(Target, SIGUSR1) == 0);
      usleep(30);
    }
  });
  for (int I = 0; I < 3000; ++I) {
    md_lease Lease{};
    assert(md_acquire(Backend, 4096, 4096, md_process_id(), &Lease) == 0);
    memset(reinterpret_cast<void *>(Lease.address), 0x55, Lease.size);
    if (I % 7 == 0) sched_yield();
    assert(*reinterpret_cast<unsigned char *>(Lease.address) == 0x55);
    assert(md_release(Backend, &Lease, 0, nullptr) == 0);
  }
  Done.store(true); Interrupt.join();
  md_stats Stats{}; md_get_stats(Backend, &Stats);
  assert(Stats.bytes_in_use == 0 && Stats.rseq_aborts > 0 && Stats.rseq_successes > 0);
  assert(Stats.remote_enqueued == Stats.remote_drained);
  printf("rseq_abort_test: PASS successes=%llu aborts=%llu remote=%llu\n",
      (unsigned long long)Stats.rseq_successes, (unsigned long long)Stats.rseq_aborts,
      (unsigned long long)Stats.remote_enqueued);
  md_backend_close(Backend);
  char Path[160]; snprintf(Path, sizeof(Path), "/dev/shm/%s_meta", Domain); unlink(Path);
  snprintf(Path, sizeof(Path), "/dev/shm/%s_data", Domain); unlink(Path);
}
