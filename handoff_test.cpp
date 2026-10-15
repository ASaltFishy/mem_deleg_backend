#include "delegation_backend.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;
static double elapsed(Clock::time_point Start) {
  return std::chrono::duration<double, std::nano>(Clock::now() - Start).count();
}
static void transfer(int Fd, void *Data, size_t Size, bool Write) {
  auto *Bytes = static_cast<unsigned char *>(Data);
  while (Size) {
    ssize_t Count = Write ? write(Fd, Bytes, Size) : read(Fd, Bytes, Size);
    assert(Count > 0);
    Bytes += Count;
    Size -= static_cast<size_t>(Count);
  }
}
static void checkZero(md_lease Lease) {
  for (size_t Index = 0; Index < Lease.size; ++Index)
    assert(reinterpret_cast<unsigned char *>(Lease.address)[Index] == 0);
}
struct Reply {
  md_candidate Candidate;
  double Acquire;
  double Publish;
};
static void print(const char *Name, std::vector<double> Values) {
  double Sum = 0;
  for (double Value : Values) Sum += Value;
  std::sort(Values.begin(), Values.end());
  printf("%s mean_ns=%.2f p50=%.0f p95=%.0f p99=%.0f\n", Name,
          Sum / Values.size(), Values[Values.size() / 2],
          Values[Values.size() * 95 / 100], Values[Values.size() * 99 / 100]);
}
int main() {
  constexpr int Rounds = 1000;
  char Domain[96];
  snprintf(Domain, sizeof(Domain), "md_handoff_%d", getpid());
  md_config Config{Domain, 0x310000000000ULL, 64ULL << 20, 0};
  md_backend *Backend;
  assert(md_backend_open(&Backend, &Config) == 0);
  md_lease Lease{};
  uint32_t Owner = md_process_id();
  assert(md_acquire(Backend, 262144, 4096, Owner, &Lease) == 0);
  uintptr_t Address = Lease.address;
  int Forward[2], Backward[2];
  assert(pipe(Forward) == 0 && pipe(Backward) == 0);
  pid_t Child = fork();
  assert(Child >= 0);
  if (Child == 0) {
    Owner = md_process_id();
    assert(Owner == static_cast<uint32_t>(getpid()));
    for (int Iteration = 0; Iteration < Rounds; ++Iteration) {
      md_candidate Candidate;
      transfer(Forward[0], &Candidate, sizeof(Candidate), false);
      auto Start = Clock::now();
      assert(md_try_acquire(Backend, Candidate, Owner, &Lease) == 0);
      double Acquire = elapsed(Start);
      assert(Lease.address == Address);
      checkZero(Lease);
      memset(reinterpret_cast<void *>(Address), 0xb2, Lease.size);
      Reply Result{};
      Start = Clock::now();
      assert(md_release(Backend, &Lease, 0, &Result.Candidate) == 0);
      Result.Publish = elapsed(Start);
      Result.Acquire = Acquire;
      transfer(Backward[1], &Result, sizeof(Result), true);
    }
    _exit(0);
  }
  std::vector<double> Publishes, Acquires, RoundTrips;
  for (int Iteration = 0; Iteration < Rounds; ++Iteration) {
    memset(reinterpret_cast<void *>(Address), 0xa1, Lease.size);
    auto Start = Clock::now();
    md_candidate Candidate;
    assert(md_release(Backend, &Lease, 0, &Candidate) == 0);
    double Publish = elapsed(Start);
    transfer(Forward[1], &Candidate, sizeof(Candidate), true);
    Reply Result;
    transfer(Backward[0], &Result, sizeof(Result), false);
    auto AcquireStart = Clock::now();
    assert(md_try_acquire(Backend, Result.Candidate, Owner, &Lease) == 0);
    double Acquire = elapsed(AcquireStart);
    assert(Lease.address == Address);
    checkZero(Lease);
    double RoundTrip = elapsed(Start);
    if (Iteration >= 100) {
      Publishes.push_back(Publish);
      Publishes.push_back(Result.Publish);
      Acquires.push_back(Acquire);
      Acquires.push_back(Result.Acquire);
      RoundTrips.push_back(RoundTrip);
    }
  }
  int Status;
  assert(waitpid(Child, &Status, 0) == Child && WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  assert(md_release(Backend, &Lease, 0, nullptr) == 0);
  print("publish", Publishes);
  print("foreign_acquire", Acquires);
  print("roundtrip_including_pipes_and_validation", RoundTrips);
  puts("handoff_test: PASS 1000 rounds, same VA, full-payload zero verification");
  md_backend_close(Backend);
}
