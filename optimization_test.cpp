#include "delegation_backend.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

static void expectZero(const md_lease &Lease) {
  for (size_t Index = 0; Index < Lease.size; ++Index)
    assert(reinterpret_cast<unsigned char *>(Lease.address)[Index] == 0);
}

int main() {
  char Domain[96];
  snprintf(Domain, sizeof(Domain), "md_opt_correctness_%d", getpid());
  md_config Config{Domain, 0x310000000000ULL, 64ULL << 20, 0};
  md_backend *Backend;
  assert(md_backend_open(&Backend, &Config) == 0);
  assert(md_process_id() == static_cast<uint32_t>(getpid()));
  md_lease First{}, Second{};
  md_candidate Candidate{};

  assert(md_acquire(Backend, 65536, 4096, 1001, &First) == 0);
  memset(reinterpret_cast<void *>(First.address), 0xa5, First.size);
  assert(md_release(Backend, &First, 0, &Candidate) == 0);
  assert(md_try_acquire(Backend, Candidate, 1001, &Second) == 0);
  assert(*reinterpret_cast<unsigned char *>(Second.address) == 0xa5);
  assert(md_release(Backend, &Second, 0, &Candidate) == 0);
  assert(md_try_acquire(Backend, Candidate, 2002, &Second) == 0);
  expectZero(Second);
  assert(md_release(Backend, &First, 0, nullptr) == ESTALE);
  assert(md_release(Backend, &Second, 0, nullptr) == 0);
  assert(md_acquire_flags(Backend, 65536, 4096, 2002, MD_ZERO, &Second) == 0);
  expectZero(Second);
  assert(md_release(Backend, &Second, 0, nullptr) == 0);

  assert(md_acquire(Backend, 32768, 4096, 1001, &First) == 0);
  assert(md_acquire(Backend, 32768, 4096, 2002, &Second) == 0);
  memset(reinterpret_cast<void *>(First.address), 0x11, First.size);
  memset(reinterpret_cast<void *>(Second.address), 0x22, Second.size);
  assert(md_release(Backend, &First, 0, &Candidate) == 0);
  assert(md_release(Backend, &Second, 0, nullptr) == 0);
  assert(md_try_acquire(Backend, Candidate, 1001, &Second) == EAGAIN);
  assert(md_acquire(Backend, 65536, 4096, 1001, &First) == 0);
  expectZero(First);
  memset(reinterpret_cast<void *>(First.address), 0x33, First.size);
  assert(md_release(Backend, &First, 0, nullptr) == 0);
  assert(md_acquire(Backend, 32768, 4096, 1001, &First) == 0);
  assert(*reinterpret_cast<unsigned char *>(First.address) == 0x33);
  assert(md_acquire(Backend, 32768, 4096, 2002, &Second) == 0);
  expectZero(Second);
  assert(md_release(Backend, &First, 0, nullptr) == 0);
  assert(md_release(Backend, &Second, 0, nullptr) == 0);

  // All returned pages, including both boundaries, belong to the allocator.
  const size_t Sizes[] = {1, 4096, 4097, 32768, 65536};
  for (size_t Size : Sizes) {
    assert(md_acquire(Backend, Size, 65536, 2002, &First) == 0);
    assert(First.address % 65536 == 0);
    assert(First.size == (Size + 4095) / 4096 * 4096);
    memset(reinterpret_cast<void *>(First.address), 0x66, First.size);
    assert(md_release(Backend, &First, 0, nullptr) == 0);
    assert(md_acquire_flags(Backend, Size, 65536, 2002, MD_ZERO, &First) == 0);
    expectZero(First);
    memset(reinterpret_cast<void *>(First.address), 0x77, First.size);
    assert(md_release(Backend, &First, 0, nullptr) == 0);
  }
  assert(md_acquire_flags(Backend, 4096, 4096, 2002, 2u, &First) == EINVAL);

  md_backend_close(Backend);
  assert(md_backend_open(&Backend, &Config) == 0);
  assert(md_acquire(Backend, 65536, 4096, 2002, &First) == 0);
  expectZero(First);
  memset(reinterpret_cast<void *>(First.address), 0x77, First.size);
  assert(md_release(Backend, &First, 0, &Candidate) == 0);
  pid_t Child = fork();
  assert(Child >= 0);
  if (Child == 0) {
    assert(md_process_id() == static_cast<uint32_t>(getpid()));
    assert(md_try_acquire(Backend, Candidate, static_cast<uint32_t>(getpid()), &First) == 0);
    expectZero(First);
    assert(md_release(Backend, &First, 0, nullptr) == 0);
    _exit(0);
  }
  int Status;
  assert(waitpid(Child, &Status, 0) == Child && WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  md_backend_close(Backend);
  puts("optimization_test: PASS same-owner/foreign-owner/calloc/mixed-merge/split/full-writable-lease/alignment/flags/session/fork");
}
