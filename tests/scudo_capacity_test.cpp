// Hold four maximum-size allocations simultaneously: sequential alloc/free
// alone cannot expose a per-arena capacity smaller than the concurrent demand.
#include "scudo_delegation.h"
#include "delegation_backend.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sched.h>
#include <unistd.h>

extern "C" int md_scudo_checkpoint();

int main(int Argc, char **Argv) {
  bool ExpectExhaustion = Argc == 2 && !std::strcmp(Argv[1], "--expect-exhaustion");
  if (Argc > 1 && !ExpectExhaustion) return 2;
  cpu_set_t Set;
  CPU_ZERO(&Set);
  CPU_SET(0, &Set);
  if (sched_setaffinity(0, sizeof(Set), &Set)) return 3;
  if (!scudo::scudoDelegationEnabled() || md_scudo_checkpoint()) return 4;
  std::printf("CAPACITY_CONFIG per_arena=%llu page=%ld\n",
              (unsigned long long)MD_DEFAULT_ARENA_CAPACITY, sysconf(_SC_PAGESIZE));
  unsigned Failures = 0;
  for (size_t Size : {size_t(128) << 20, size_t(256) << 20}) {
    void *Pointers[4]{};
    unsigned Delegated = 0, NoMemory = 0;
    for (unsigned I = 0; I < 4; ++I) {
      Pointers[I] = std::malloc(Size);
      if (!Pointers[I]) { ++Failures; continue; }
      bool Owns = scudo::scudoDelegationContains((scudo::uptr)Pointers[I]);
      if (Owns) {
        ++Delegated;
        std::printf("CAPACITY_ALLOC size=%zu held=%u route=delegated\n", Size, I+1);
      } else {
        auto Failure = scudo::scudoDelegationLastFailure();
        if (Failure.Error == ENOMEM) ++NoMemory;
        std::printf("CAPACITY_ALLOC size=%zu held=%u route=fallback error=%d requested=%zu\n",
                    Size, I+1, Failure.Error, (size_t)Failure.RequestedSize);
      }
      // Fault at both ends while all previous leases remain held.
      auto *Bytes = static_cast<volatile unsigned char *>(Pointers[I]);
      Bytes[0] = (unsigned char)I;
      Bytes[Size-1] = (unsigned char)I;
    }
    for (unsigned I = 0; I < 4; ++I) {
      if (!Pointers[I]) continue;
      auto *Bytes = static_cast<volatile unsigned char *>(Pointers[I]);
      if (Bytes[0] != I || Bytes[Size-1] != I) ++Failures;
      std::free(Pointers[I]);
    }
    if (md_scudo_checkpoint()) ++Failures;
    if (ExpectExhaustion ? (NoMemory == 0 || Delegated + NoMemory != 4) : Delegated != 4)
      ++Failures;
    std::printf("CAPACITY_CASE size=%zu delegated=%u enomem=%u\n", Size, Delegated, NoMemory);
  }
  if (Failures) return 1;
  std::puts(ExpectExhaustion ? "CAPACITY_EXHAUSTION_REPRODUCED" : "SCUDO_CAPACITY_ALL_PASS");
  return 0;
}
