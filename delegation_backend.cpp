#include "delegation_backend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/rseq.h>
#include <time.h>
#include <linux/futex.h>
#include <sys/syscall.h>

inline void *operator new(size_t, void *Pointer) noexcept { return Pointer; }

namespace md_atomic {
constexpr int memory_order_relaxed = __ATOMIC_RELAXED;
constexpr int memory_order_acquire = __ATOMIC_ACQUIRE;
constexpr int memory_order_release = __ATOMIC_RELEASE;
template <typename Value> struct Atomic {
  Value Storage;
  Value load(int Order = __ATOMIC_SEQ_CST) const {
    return __atomic_load_n(&Storage, Order);
  }
  void store(Value NewValue, int Order = __ATOMIC_SEQ_CST) {
    __atomic_store_n(&Storage, NewValue, Order);
  }
  Value exchange(Value V, int Order = __ATOMIC_SEQ_CST) {
    return __atomic_exchange_n(&Storage, V, Order);
  }
  Value incrementLocked() {
    Value Previous = load(memory_order_relaxed);
    store(Previous + 1, memory_order_relaxed);
    return Previous;
  }
  bool compare_exchange_weak(Value &Expected, Value Desired, int Success,
                              int Failure) {
    return __atomic_compare_exchange_n(&Storage, &Expected, Desired, true,
                                       Success, Failure);
  }
};
inline void atomic_thread_fence(int Order) { __atomic_thread_fence(Order); }
using ::memset;
}

namespace {

constexpr uint64_t kMagic = 0x4d4442454e443031ULL;
constexpr uint32_t kVersion = 3;
constexpr uint32_t kNoIndex = UINT32_MAX;
constexpr uint32_t kNoOwner = UINT32_MAX;
constexpr uint32_t kMaxRuns = 8192;
constexpr uint32_t kNumBins = 24;

enum RunState : uint32_t { Unused, Free, Claiming, InUse, Pending };

struct alignas(64) RunDescriptor {
  md_atomic::Atomic<uint32_t> State;
  md_atomic::Atomic<uint64_t> Generation;
  md_atomic::Atomic<uint64_t> ReleaseTicket;
  uint32_t Owner;
  uint32_t StartPage;
  uint32_t Pages;
  uint32_t Prev;
  uint32_t Next;
  uint32_t BinPrev;
  uint32_t BinNext;
  uint64_t LastSession;
  uint32_t LastOwner;
  uint32_t QueueNext;
};

struct alignas(64) SharedMetadata {
  uint64_t Magic;
  uint32_t Version;
  uint32_t PageSize;
  uint64_t Capacity;
  uintptr_t FixedAddress;
  md_atomic::Atomic<uint32_t> Lock;
  pthread_mutex_t QueueLock;
  md_atomic::Atomic<uint32_t> QueueHead;
  uint32_t QueueTail;
  uint32_t DescriptorHead;
  uint32_t NextUnused;
  uint32_t InsertHint;
  uint32_t ArenaId;
  uint32_t ArenaCount;
  uint32_t BumpPage;
  uint32_t FreeHead;
  uint32_t BinHeads[kNumBins];
  uint64_t NextSession;
  md_atomic::Atomic<uint64_t> AcquireCalls;
  md_atomic::Atomic<uint64_t> AcquireSuccesses;
  md_atomic::Atomic<uint64_t> ReleaseCalls;
  md_atomic::Atomic<uint64_t> ReleaseSuccesses;
  md_atomic::Atomic<uint64_t> SplitCount;
  md_atomic::Atomic<uint64_t> MergeCount;
  uint64_t LockAcquisitions;
  uint64_t LockContentions;
  uint64_t LockWaitNs;
  uint64_t RemoteEnqueued;
  uint64_t RemoteDrained;
  uint64_t RseqSuccesses;
  uint64_t RseqAborts;
  uint64_t DescriptorPops;
  uint64_t AddressSteps;
  uint64_t HintHits;
  uint64_t QueueContentions;
  uint64_t QueueWaitNs;
  RunDescriptor Runs[kMaxRuns];
};

struct BackendImpl {
  int MetadataFd = -1;
  int PayloadFd = -1;
  SharedMetadata *Metadata = nullptr;
  unsigned char *Payload = nullptr;
  size_t Capacity = 0;
  size_t PageSize = 0;
  uint64_t Session = 0;
  bool EagerClear = false;
  bool UseRseq = true;
  size_t MetadataSize = 0;
  uint32_t ArenaCount = 1;
  bool Prelocked = false;
};

static uint32_t sizeClass(uint32_t Pages) {
  uint32_t Class = 0;
  uint32_t Value = Pages > 0 ? Pages - 1 : 0;
  while (Value != 0 && Class + 1 < kNumBins) {
    Value >>= 1;
    ++Class;
  }
  return Class;
}

static uint64_t nowNs() {
  timespec Time;
  clock_gettime(CLOCK_MONOTONIC, &Time);
  return uint64_t(Time.tv_sec) * 1000000000 + uint64_t(Time.tv_nsec);
}

static void queueLock(SharedMetadata *Metadata) {
  if (pthread_mutex_trylock(&Metadata->QueueLock) == 0) return;
  uint64_t Start = nowNs();
  pthread_mutex_lock(&Metadata->QueueLock);
  ++Metadata->QueueContentions;
  Metadata->QueueWaitNs += nowNs() - Start;
}

static void lock(SharedMetadata *Metadata) {
  uint32_t Expected = 0;
  if (Metadata->Lock.compare_exchange_weak(Expected, 1, __ATOMIC_ACQUIRE,
                                           __ATOMIC_RELAXED)) {
    ++Metadata->LockAcquisitions;
    return;
  }
  uint64_t Start = nowNs();
  while (Metadata->Lock.exchange(2, __ATOMIC_ACQUIRE) != 0)
    syscall(SYS_futex, &Metadata->Lock.Storage, FUTEX_WAIT, 2, nullptr, nullptr, 0);
  ++Metadata->LockAcquisitions;
  ++Metadata->LockContentions;
  Metadata->LockWaitNs += nowNs() - Start;
}

static void unlock(SharedMetadata *Metadata) {
  if (Metadata->Lock.exchange(0, __ATOMIC_RELEASE) == 2)
    syscall(SYS_futex, &Metadata->Lock.Storage, FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

// The only shared write inside the rseq interval is the committing lock CAS.
// Split/merge runs after commit and uses the same futex lock as fallback paths.
static int rseqLock(SharedMetadata *Metadata, uint32_t Cpu) {
#if defined(__x86_64__) && defined(__GLIBC__)
  if (__rseq_size < 20) return -2;
  auto *Area = reinterpret_cast<struct rseq *>(
      static_cast<char *>(__builtin_thread_pointer()) + __rseq_offset);
  int Result;
  asm volatile(
      ".pushsection __rseq_cs, \"aw\"\n"
      ".balign 32\n"
      "4: .long 0, 0\n .quad 1f, 2f-1f, 3f\n"
      ".popsection\n"
      "leaq 4b(%%rip), %%rax\n"
      "movq %%rax, 8(%[area])\n"
      "1: cmpl %[cpu], 4(%[area])\n"
      "jne 7f\n"
#ifdef MD_TEST_RSEQ_DELAY
      "movl $100000, %%ecx\n"
      "6: decl %%ecx\n jnz 6b\n"
#endif
      "xorl %%eax, %%eax\n"
      "movl $1, %%edx\n"
      "lock cmpxchgl %%edx, (%[gate])\n"
      "2: setnz %%al\n"
      "movzbl %%al, %[result]\n"
      "jmp 5f\n"
      ".balign 4\n .long 0x53053053\n"
      "3: movl $-1, %[result]\n"
      "jmp 5f\n"
      "7: movl $-3, %[result]\n"
      "5: movq $0, 8(%[area])\n"
      : [result] "=&r"(Result)
      : [area] "r"(Area), [gate] "r"(&Metadata->Lock.Storage), [cpu] "r"(Cpu)
      : "rax", "rdx", "rcx", "memory", "cc");
  return Result;
#else
  (void)Metadata; (void)Cpu;
  return -2;
#endif
}

static uint32_t allocateDescriptor(SharedMetadata *Metadata) {
  uint32_t Index = Metadata->DescriptorHead;
  if (Index == kNoIndex) {
    if (Metadata->NextUnused == kMaxRuns) return kNoIndex;
    Index = Metadata->NextUnused++;
  } else {
    Metadata->DescriptorHead = Metadata->Runs[Index].Next;
  }
  Metadata->Runs[Index].State.store(Claiming, md_atomic::memory_order_relaxed);
  ++Metadata->DescriptorPops;
  return Index;
}

static void unlinkBin(SharedMetadata *Metadata, uint32_t Index) {
  RunDescriptor &Run = Metadata->Runs[Index];
  uint32_t Class = sizeClass(Run.Pages);
  if (Run.BinPrev != kNoIndex)
    Metadata->Runs[Run.BinPrev].BinNext = Run.BinNext;
  else
    Metadata->BinHeads[Class] = Run.BinNext;
  if (Run.BinNext != kNoIndex)
    Metadata->Runs[Run.BinNext].BinPrev = Run.BinPrev;
  Run.BinPrev = kNoIndex;
  Run.BinNext = kNoIndex;
}

static void linkBin(SharedMetadata *Metadata, uint32_t Index) {
  RunDescriptor &Run = Metadata->Runs[Index];
  uint32_t Class = sizeClass(Run.Pages);
  Run.BinPrev = kNoIndex;
  Run.BinNext = Metadata->BinHeads[Class];
  if (Run.BinNext != kNoIndex)
    Metadata->Runs[Run.BinNext].BinPrev = Index;
  Metadata->BinHeads[Class] = Index;
}

static void unlinkAddress(SharedMetadata *Metadata, uint32_t Index) {
  RunDescriptor &Run = Metadata->Runs[Index];
  if (Metadata->InsertHint == Index)
    Metadata->InsertHint = Run.Prev != kNoIndex ? Run.Prev : Run.Next;
  if (Run.Prev != kNoIndex)
    Metadata->Runs[Run.Prev].Next = Run.Next;
  else
    Metadata->FreeHead = Run.Next;
  if (Run.Next != kNoIndex)
    Metadata->Runs[Run.Next].Prev = Run.Prev;
  Run.Prev = kNoIndex;
  Run.Next = kNoIndex;
}

static void linkAddress(SharedMetadata *Metadata, uint32_t Index) {
  RunDescriptor &Run = Metadata->Runs[Index];
  uint32_t Prev = kNoIndex;
  uint32_t Next = Metadata->FreeHead;
  uint32_t Hint = Metadata->InsertHint;
  if (Hint != kNoIndex && Metadata->Runs[Hint].StartPage < Run.StartPage) {
    Prev = Hint;
    Next = Metadata->Runs[Hint].Next;
    ++Metadata->HintHits;
  }
  while (Next != kNoIndex &&
         Metadata->Runs[Next].StartPage < Run.StartPage) {
    Prev = Next;
    Next = Metadata->Runs[Next].Next;
    ++Metadata->AddressSteps;
  }
  Run.Prev = Prev;
  Run.Next = Next;
#ifdef MD_TEST_PREEMPT_LOCK
  // Force a same-CPU handoff while list publication is still incomplete.
  usleep(50);
#endif
  if (Prev != kNoIndex)
    Metadata->Runs[Prev].Next = Index;
  else
    Metadata->FreeHead = Index;
  if (Next != kNoIndex)
    Metadata->Runs[Next].Prev = Index;
  Metadata->InsertHint = Index;
}

static void initializeRun(SharedMetadata *Metadata, uint32_t Index,
                          uint32_t StartPage, uint32_t Pages,
                          RunState StateValue) {
  RunDescriptor &Run = Metadata->Runs[Index];
  Run.Owner = kNoOwner;
  Run.LastOwner = kNoOwner;
  Run.LastSession = 0;
  Run.StartPage = StartPage;
  Run.Pages = Pages;
  Run.Prev = kNoIndex;
  Run.Next = kNoIndex;
  Run.BinPrev = kNoIndex;
  Run.BinNext = kNoIndex;
  Run.Generation.incrementLocked();
  Run.State.store(StateValue, md_atomic::memory_order_release);
}

static void makeFree(SharedMetadata *Metadata, uint32_t Index,
                     uint32_t StartPage, uint32_t Pages) {
  initializeRun(Metadata, Index, StartPage, Pages, Free);
  linkAddress(Metadata, Index);
  linkBin(Metadata, Index);
}

static void retireRun(SharedMetadata *Metadata, uint32_t Index) {
  RunDescriptor &Run = Metadata->Runs[Index];
  Run.Generation.incrementLocked();
  Run.Owner = kNoOwner;
  Run.Pages = 0;
  Run.Prev = Run.Next = Run.BinPrev = Run.BinNext = kNoIndex;
  Run.State.store(Unused, md_atomic::memory_order_release);
  Run.Next = Metadata->DescriptorHead;
  Metadata->DescriptorHead = Index;
}

static void mergeProvenance(RunDescriptor &Destination,
                            const RunDescriptor &Source) {
  if (Source.LastSession == 0) return;
  if (Destination.LastSession == 0) {
    Destination.LastSession = Source.LastSession;
    Destination.LastOwner = Source.LastOwner;
  } else if (Destination.LastSession != Source.LastSession ||
             Destination.LastOwner != Source.LastOwner) {
    Destination.LastSession = UINT64_MAX;
    Destination.LastOwner = kNoOwner;
  }
}

static uint32_t mergeFreeRun(SharedMetadata *Metadata, uint32_t Index) {
  RunDescriptor *Run = &Metadata->Runs[Index];
  uint32_t Prev = Run->Prev;
  if (Prev != kNoIndex) {
    RunDescriptor &PrevRun = Metadata->Runs[Prev];
    if (PrevRun.StartPage + PrevRun.Pages == Run->StartPage) {
      mergeProvenance(PrevRun, *Run);
      unlinkBin(Metadata, Prev);
      unlinkBin(Metadata, Index);
      unlinkAddress(Metadata, Index);
      PrevRun.Pages += Run->Pages;
      PrevRun.Generation.incrementLocked();
      retireRun(Metadata, Index);
      linkBin(Metadata, Prev);
      Metadata->MergeCount.incrementLocked();
      Index = Prev;
      Run = &Metadata->Runs[Index];
    }
  }
  uint32_t Next = Run->Next;
  if (Next != kNoIndex) {
    RunDescriptor &NextRun = Metadata->Runs[Next];
    if (Run->StartPage + Run->Pages == NextRun.StartPage) {
      mergeProvenance(*Run, NextRun);
      unlinkBin(Metadata, Index);
      unlinkBin(Metadata, Next);
      unlinkAddress(Metadata, Next);
      Run->Pages += NextRun.Pages;
      Run->Generation.incrementLocked();
      retireRun(Metadata, Next);
      linkBin(Metadata, Index);
      Metadata->MergeCount.incrementLocked();
    }
  }
  return Index;
}

static bool alignedStart(uint32_t StartPage, uint32_t Pages,
                         uint32_t NeededPages, uint32_t AlignmentPages,
                         uint32_t *Result) {
  uint32_t Mask = AlignmentPages - 1;
  uint32_t Start = (StartPage + Mask) & ~Mask;
  if (Start < StartPage || Start - StartPage > Pages ||
      NeededPages > Pages - (Start - StartPage))
    return false;
  *Result = Start;
  return true;
}

static int claimRun(BackendImpl *Backend, uint32_t Index, uint32_t StartPage,
                    uint32_t NeededPages, uint32_t Owner, md_lease *Out) {
  SharedMetadata *Metadata = Backend->Metadata;
  RunDescriptor &Run = Metadata->Runs[Index];
  uint32_t OriginalStart = Run.StartPage;
  uint32_t OriginalPages = Run.Pages;
  uint64_t LastSession = Run.LastSession;
  uint32_t LastOwner = Run.LastOwner;
  uint32_t PrefixPages = StartPage - OriginalStart;
  uint32_t SuffixStart = StartPage + NeededPages;
  uint32_t SuffixPages = OriginalStart + OriginalPages - SuffixStart;
  uint32_t Prefix = kNoIndex;
  uint32_t Suffix = kNoIndex;
  if (PrefixPages != 0) {
    Prefix = allocateDescriptor(Metadata);
    if (Prefix == kNoIndex)
      return -1;
    Metadata->Runs[Prefix].State.store(Claiming, md_atomic::memory_order_relaxed);
  }
  if (SuffixPages != 0) {
    Suffix = allocateDescriptor(Metadata);
    if (Suffix == kNoIndex) {
      if (Prefix != kNoIndex)
        retireRun(Metadata, Prefix);
      return -1;
    }
  }
  unlinkBin(Metadata, Index);
  unlinkAddress(Metadata, Index);
  if (Prefix != kNoIndex) {
    makeFree(Metadata, Prefix, OriginalStart, PrefixPages);
    Metadata->Runs[Prefix].LastSession = LastSession;
    Metadata->Runs[Prefix].LastOwner = LastOwner;
    Metadata->SplitCount.incrementLocked();
  }
  if (Suffix != kNoIndex) {
    makeFree(Metadata, Suffix, SuffixStart, SuffixPages);
    Metadata->Runs[Suffix].LastSession = LastSession;
    Metadata->Runs[Suffix].LastOwner = LastOwner;
    Metadata->SplitCount.incrementLocked();
  }
  Run.StartPage = StartPage;
  Run.Pages = NeededPages;
  Run.Owner = Owner;
  uint64_t Generation =
      Run.Generation.incrementLocked() + 1;
  Run.State.store(Claiming, md_atomic::memory_order_release);
  Out->block_id = Index;
  Out->owner = Owner;
  Out->generation = Generation;
  Out->address = reinterpret_cast<uintptr_t>(Backend->Payload) +
                 static_cast<uintptr_t>(StartPage) * Backend->PageSize;
  Out->size = static_cast<size_t>(NeededPages) * Backend->PageSize;
  return 0;
}

static int finishAcquire(BackendImpl &Impl, const md_lease &Lease, unsigned Flags) {
  RunDescriptor &Run = Impl.Metadata->Runs[Lease.block_id];
  bool Clear = (Flags & MD_ZERO) ||
      (Run.LastSession != 0 && (Run.LastSession != Impl.Session ||
                               Run.LastOwner != Lease.owner));
  if (Clear)
    memset(reinterpret_cast<void *>(Lease.address), 0, Lease.size);
  Run.State.store(InUse, md_atomic::memory_order_release);
  Run.ReleaseTicket.store(Lease.generation, __ATOMIC_RELEASE);
  return 0;
}

} // namespace

struct md_backend { BackendImpl Impl; };

static md_backend arenaView(md_backend *Backend, uint32_t Arena) {
  md_backend View = *Backend;
  View.Impl.Metadata += Arena;
  View.Impl.Payload += static_cast<size_t>(Arena) * View.Impl.Capacity;
  return View;
}

static uint32_t publishLocked(SharedMetadata *Metadata, uint32_t Index) {
  RunDescriptor &Run = Metadata->Runs[Index];
  Run.Owner = kNoOwner;
  Run.Generation.incrementLocked();
  Run.State.store(Free, __ATOMIC_RELEASE);
  Run.Prev = Run.Next = Run.BinPrev = Run.BinNext = kNoIndex;
  linkAddress(Metadata, Index);
  linkBin(Metadata, Index);
  return mergeFreeRun(Metadata, Index);
}

static void drainLocked(SharedMetadata *Metadata) {
  if (Metadata->QueueHead.load(__ATOMIC_ACQUIRE) == kNoIndex) return;
  queueLock(Metadata);
  uint32_t Head = Metadata->QueueHead.exchange(kNoIndex, __ATOMIC_RELAXED);
  Metadata->QueueTail = kNoIndex;
  pthread_mutex_unlock(&Metadata->QueueLock);
  while (Head != kNoIndex) {
    uint32_t Next = Metadata->Runs[Head].QueueNext;
    publishLocked(Metadata, Head);
    ++Metadata->RemoteDrained;
    Metadata->ReleaseCalls.incrementLocked();
    Metadata->ReleaseSuccesses.incrementLocked();
    Head = Next;
  }
}

static thread_local uint32_t CachedPid;
static void afterForkChild() { CachedPid = 0; }
extern "C" uint32_t md_process_id() {
  if (!CachedPid) CachedPid = static_cast<uint32_t>(getpid());
  return CachedPid;
}

extern "C" int md_backend_open(md_backend **Out, const md_config *Config) {
  if (Out == nullptr || Config == nullptr || Config->domain == nullptr ||
      Config->fixed_address == 0 || Config->capacity == 0)
    return EINVAL;
  *Out = nullptr;
  size_t PageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  size_t Capacity = (Config->capacity / PageSize) * PageSize;
  long CpuCount = sysconf(_SC_NPROCESSORS_CONF);
  if (CpuCount <= 0 || CpuCount > 1024) return ENOTSUP;
  // Single-arena mode is only a contention-control experiment, not the default.
  const char *Single = getenv("MD_SINGLE_ARENA");
  uint32_t ArenaCount = Single && Single[0] == '1' ? 1 : static_cast<uint32_t>(CpuCount);
  size_t MetadataSize = sizeof(SharedMetadata) * ArenaCount;
  if (Capacity > SIZE_MAX / ArenaCount) return EINVAL;
  size_t WindowSize = Capacity * ArenaCount;
  if (Config->fixed_address > UINTPTR_MAX - WindowSize) return EINVAL;
  if (strlen(Config->domain) > 120 || strchr(Config->domain, '/') ||
      Capacity == 0 || Capacity / PageSize > UINT32_MAX ||
      Config->fixed_address % Capacity != 0)
    return EINVAL;
  char MetadataName[160], PayloadName[160];
  snprintf(MetadataName, sizeof(MetadataName), "/%s_meta", Config->domain);
  snprintf(PayloadName, sizeof(PayloadName), "/%s_data", Config->domain);
  int MetadataFd = shm_open(MetadataName, O_RDWR | O_CREAT | O_EXCL, 0600);
  bool Created = MetadataFd >= 0;
  if (!Created && errno == EEXIST)
    MetadataFd = shm_open(MetadataName, O_RDWR, 0600);
  if (MetadataFd < 0) return errno;
#ifdef MD_TEST_FIRST_OPEN_DELAY
  // Force the name creator to lose the initialization-lock race in tests.
  const char *CreatorGate = getenv("MD_TEST_CREATOR_GATE_FD");
  if (Created && CreatorGate) {
    char Token;
    ssize_t Count;
    do {
      Count = read(atoi(CreatorGate), &Token, 1);
    } while (Count < 0 && errno == EINTR);
    if (Count != 1) {
      int Error = Count < 0 ? errno : EIO;
      close(MetadataFd);
      return Error;
    }
  }
#endif
  int PayloadFd = -1;
  SharedMetadata *Metadata = nullptr;
  void *Payload = MAP_FAILED;
  void *Storage = MAP_FAILED;
  auto Fail = [&](int Error) {
    if (Storage != MAP_FAILED) munmap(Storage, sizeof(md_backend));
    if (Payload != MAP_FAILED) munmap(Payload, WindowSize);
    if (Metadata) munmap(Metadata, MetadataSize);
    if (PayloadFd >= 0) close(PayloadFd);
    // Never unlink here: waiters may already hold this inode open.
    close(MetadataFd);
    return Error;
  };
  int LockResult;
  do {
    LockResult = flock(MetadataFd, LOCK_EX);
  } while (LockResult != 0 && errno == EINTR);
  if (LockResult != 0) return Fail(errno);
  struct stat MetaStat{}, DataStat{};
  if (fstat(MetadataFd, &MetaStat) != 0) return Fail(errno);
  // The first lock holder initializes the empty inode, even if another
  // process created its name. File creation alone does not elect an initializer.
  bool Initialize = MetaStat.st_size == 0;
  if (!Initialize && (Config->reset ||
                      MetaStat.st_size != static_cast<off_t>(MetadataSize)))
    return Fail(EINVAL);
  PayloadFd = shm_open(PayloadName, O_RDWR | (Initialize ? O_CREAT : 0), 0600);
  if (PayloadFd < 0) return Fail(errno);
  if (Initialize) {
    if (ftruncate(PayloadFd, static_cast<off_t>(WindowSize)) != 0 ||
        ftruncate(MetadataFd, static_cast<off_t>(MetadataSize)) != 0)
      return Fail(errno);
  } else {
    if (fstat(PayloadFd, &DataStat) != 0) return Fail(errno);
    if (DataStat.st_size != static_cast<off_t>(WindowSize)) return Fail(EINVAL);
  }
  Metadata = static_cast<SharedMetadata *>(
      mmap(nullptr, MetadataSize, PROT_READ | PROT_WRITE, MAP_SHARED,
           MetadataFd, 0));
  if (Metadata == MAP_FAILED) {
    int Error = errno;
    Metadata = nullptr;
    return Fail(Error);
  }
  if (Initialize) {
    pthread_mutexattr_t Attributes;
    int Error = pthread_mutexattr_init(&Attributes);
    if (Error) return Fail(Error);
    Error = pthread_mutexattr_setpshared(&Attributes, PTHREAD_PROCESS_SHARED);
    if (Error) {
      pthread_mutexattr_destroy(&Attributes);
      return Fail(Error);
    }
    for (uint32_t Arena = 0; Arena < ArenaCount; ++Arena) {
      SharedMetadata *Current = Metadata + Arena;
      Current->ArenaId = Arena;
      Current->ArenaCount = ArenaCount;
      Current->FreeHead = Current->InsertHint = kNoIndex;
      Current->QueueHead.store(kNoIndex, __ATOMIC_RELAXED);
      Current->QueueTail = kNoIndex;
      Current->DescriptorHead = kNoIndex;
      Error = pthread_mutex_init(&Current->QueueLock, &Attributes);
      if (Error) {
        pthread_mutexattr_destroy(&Attributes);
        return Fail(Error);
      }
      for (uint32_t &Head : Current->BinHeads) Head = kNoIndex;
    }
    pthread_mutexattr_destroy(&Attributes);
    Metadata->Version = kVersion;
    Metadata->PageSize = static_cast<uint32_t>(PageSize);
    Metadata->Capacity = Capacity;
    Metadata->FixedAddress = Config->fixed_address;
    md_atomic::atomic_thread_fence(md_atomic::memory_order_release);
    Metadata->Magic = kMagic;
  } else if (Metadata->Magic != kMagic || Metadata->Version != kVersion ||
             Metadata->Capacity != Capacity ||
             Metadata->FixedAddress != Config->fixed_address ||
             Metadata->PageSize != PageSize || Metadata->ArenaCount != ArenaCount) {
    return Fail(EINVAL);
  }
  Payload = mmap(reinterpret_cast<void *>(Config->fixed_address),
                       WindowSize, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED_NOREPLACE, PayloadFd, 0);
  if (Payload == MAP_FAILED) return Fail(errno);
  uint64_t Session = ++Metadata->NextSession;
  flock(MetadataFd, LOCK_UN);
  Storage = mmap(nullptr, sizeof(md_backend), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Storage == MAP_FAILED) return Fail(errno);
  auto *Backend = new (Storage) md_backend;
  Backend->Impl.MetadataFd = MetadataFd;
  Backend->Impl.PayloadFd = PayloadFd;
  Backend->Impl.Metadata = Metadata;
  Backend->Impl.Payload = static_cast<unsigned char *>(Payload);
  Backend->Impl.Capacity = Capacity;
  Backend->Impl.PageSize = PageSize;
  Backend->Impl.Session = Session;
  Backend->Impl.MetadataSize = MetadataSize;
  Backend->Impl.ArenaCount = ArenaCount;
  const char *UseRseq = getenv("MD_RSEQ");
  Backend->Impl.UseRseq = !UseRseq || UseRseq[0] != '0';
  const char *Eager = getenv("MD_EAGER_CLEAR");
  Backend->Impl.EagerClear = Eager && Eager[0] == '1';
  int Error = pthread_atfork(nullptr, nullptr, afterForkChild);
  if (Error) {
    Backend->~md_backend();
    return Fail(Error);
  }
  *Out = Backend;
  return 0;
}

extern "C" void md_backend_close(md_backend *Backend) {
  if (Backend == nullptr)
    return;
  munmap(Backend->Impl.Payload, Backend->Impl.Capacity * Backend->Impl.ArenaCount);
  munmap(Backend->Impl.Metadata, Backend->Impl.MetadataSize);
  close(Backend->Impl.PayloadFd);
  close(Backend->Impl.MetadataFd);
  Backend->~md_backend();
  munmap(Backend, sizeof(md_backend));
}

extern "C" int md_acquire(md_backend *Backend, size_t Size, size_t Alignment,
                            uint32_t Owner, md_lease *Out) {
  return md_acquire_flags(Backend, Size, Alignment, Owner, 0, Out);
}

static int acquireLocal(md_backend *Backend, size_t Size, size_t Alignment,
                                  uint32_t Owner, unsigned Flags, md_lease *Out) {
  if (Backend == nullptr || Out == nullptr || Size == 0 ||
      Size > Backend->Impl.Capacity || Alignment > Backend->Impl.Capacity ||
      (Alignment && (Alignment & (Alignment - 1))) || Owner == kNoOwner ||
      (Flags & ~static_cast<unsigned>(MD_ZERO)))
    return EINVAL;
  BackendImpl &Impl = Backend->Impl;
  SharedMetadata *Metadata = Impl.Metadata;
  uint32_t NeededPages = static_cast<uint32_t>(
      (Size + Impl.PageSize - 1) / Impl.PageSize);
  uint32_t AlignmentPages = static_cast<uint32_t>(
      (Alignment + Impl.PageSize - 1) / Impl.PageSize);
  if (AlignmentPages == 0)
    AlignmentPages = 1;
  uint32_t PowerOfTwo = 1;
  while (PowerOfTwo < AlignmentPages)
    PowerOfTwo <<= 1;
  AlignmentPages = PowerOfTwo;
  if (!Impl.Prelocked) lock(Metadata);
  drainLocked(Metadata);
  Metadata->AcquireCalls.incrementLocked();
  uint32_t StartClass = sizeClass(NeededPages);
  for (uint32_t Class = StartClass; Class < kNumBins; ++Class) {
    for (uint32_t Index = Metadata->BinHeads[Class]; Index != kNoIndex;
         Index = Metadata->Runs[Index].BinNext) {
      RunDescriptor &Run = Metadata->Runs[Index];
      uint32_t StartPage;
      if (Run.State.load(md_atomic::memory_order_acquire) == Free &&
          alignedStart(Run.StartPage, Run.Pages, NeededPages, AlignmentPages,
                       &StartPage)) {
        int Result = claimRun(&Impl, Index, StartPage, NeededPages, Owner, Out);
        if (Result == 0)
          Metadata->AcquireSuccesses.incrementLocked();
        unlock(Metadata);
        return Result == 0 ? finishAcquire(Impl, *Out, Flags) : ENOSPC;
      }
    }
  }
  uint32_t StartPage =
      (Metadata->BumpPage + AlignmentPages - 1) & ~(AlignmentPages - 1);
  uint32_t TotalPages = static_cast<uint32_t>(Impl.Capacity / Impl.PageSize);
  if (StartPage > TotalPages || NeededPages > TotalPages - StartPage) {
    unlock(Metadata);
    return ENOMEM;
  }
  if (StartPage != Metadata->BumpPage) {
    uint32_t Prefix = allocateDescriptor(Metadata);
    if (Prefix == kNoIndex) {
      unlock(Metadata);
      return ENOSPC;
    }
    makeFree(Metadata, Prefix, Metadata->BumpPage,
             StartPage - Metadata->BumpPage);
    Metadata->BumpPage = StartPage;
  }
  uint32_t Index = allocateDescriptor(Metadata);
  if (Index == kNoIndex) {
    unlock(Metadata);
    return ENOSPC;
  }
  initializeRun(Metadata, Index, StartPage, NeededPages, Claiming);
  Metadata->BumpPage = StartPage + NeededPages;
  RunDescriptor &Run = Metadata->Runs[Index];
  Run.Owner = Owner;
  uint64_t Generation =
      Run.Generation.incrementLocked() + 1;
  Run.State.store(Claiming, md_atomic::memory_order_release);
  Out->block_id = Index;
  Out->owner = Owner;
  Out->generation = Generation;
  Out->address = reinterpret_cast<uintptr_t>(Impl.Payload) +
                 static_cast<uintptr_t>(StartPage) * Impl.PageSize;
  Out->size = static_cast<size_t>(NeededPages) * Impl.PageSize;
  Metadata->AcquireSuccesses.incrementLocked();
  unlock(Metadata);
  return finishAcquire(Impl, *Out, Flags);
}

static int tryAcquireLocal(md_backend *Backend, md_candidate Candidate,
                                uint32_t Owner, md_lease *Out) {
  if (Backend == nullptr || Out == nullptr || Candidate.block_id >= kMaxRuns)
    return EINVAL;
  SharedMetadata *Metadata = Backend->Impl.Metadata;
  lock(Metadata);
  if (Backend->Impl.ArenaCount > 1 && sched_getcpu() != static_cast<int>(Metadata->ArenaId)) {
    unlock(Metadata);
    return EAGAIN;
  }
  drainLocked(Metadata);
  Metadata->AcquireCalls.incrementLocked();
  RunDescriptor &Run = Metadata->Runs[Candidate.block_id];
  if (Run.State.load(md_atomic::memory_order_acquire) != Free ||
      Run.Generation.load(md_atomic::memory_order_acquire) != Candidate.generation) {
    unlock(Metadata);
    return EAGAIN;
  }
  unlinkBin(Metadata, Candidate.block_id);
  unlinkAddress(Metadata, Candidate.block_id);
  Run.Owner = Owner;
  uint64_t Generation =
      Run.Generation.incrementLocked() + 1;
  Run.State.store(Claiming, md_atomic::memory_order_release);
  Out->block_id = Candidate.block_id;
  Out->owner = Owner;
  Out->generation = Generation;
  Out->address = reinterpret_cast<uintptr_t>(Backend->Impl.Payload) +
                 static_cast<uintptr_t>(Run.StartPage) * Backend->Impl.PageSize;
  Out->size = static_cast<size_t>(Run.Pages) * Backend->Impl.PageSize;
  Metadata->AcquireSuccesses.incrementLocked();
  unlock(Metadata);
  return finishAcquire(Backend->Impl, *Out, 0);
}

static int releaseLocal(md_backend *Backend, const md_lease *Lease,
                            int ClearPayload, md_candidate *OutCandidate) {
  if (Backend == nullptr || Lease == nullptr || Lease->block_id >= kMaxRuns)
    return EINVAL;
  BackendImpl &Impl = Backend->Impl;
  SharedMetadata *Metadata = Impl.Metadata;
  lock(Metadata);
  drainLocked(Metadata);
  Metadata->ReleaseCalls.incrementLocked();
  RunDescriptor &Run = Metadata->Runs[Lease->block_id];
  uint64_t Expected = Lease->generation;
  if (!Expected || !__atomic_compare_exchange_n(&Run.ReleaseTicket.Storage, &Expected, uint64_t(0),
                                    false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    unlock(Metadata);
    return ESTALE;
  }
  if (Run.Owner != Lease->owner ||
      Lease->size != static_cast<size_t>(Run.Pages) * Impl.PageSize ||
      Run.Generation.load(md_atomic::memory_order_acquire) != Lease->generation ||
      Lease->address != reinterpret_cast<uintptr_t>(Impl.Payload) +
                            static_cast<uintptr_t>(Run.StartPage) * Impl.PageSize) {
    Run.ReleaseTicket.store(Lease->generation, __ATOMIC_RELEASE);
    unlock(Metadata);
    return ESTALE;
  }
  ClearPayload = ClearPayload || Impl.EagerClear;
  Run.State.store(Claiming, __ATOMIC_RELEASE);
  if (ClearPayload) {
    unlock(Metadata);
    memset(reinterpret_cast<void *>(Lease->address), 0, Lease->size);
    lock(Metadata);
  }
  Run.LastOwner = ClearPayload ? kNoOwner : Lease->owner;
  Run.LastSession = ClearPayload ? 0 : Impl.Session;
  uint32_t ResultIndex = publishLocked(Metadata, Lease->block_id);
  if (OutCandidate != nullptr) {
    OutCandidate->block_id = ResultIndex;
    OutCandidate->generation =
        Metadata->Runs[ResultIndex].Generation.load(md_atomic::memory_order_acquire);
  }
  Metadata->ReleaseSuccesses.incrementLocked();
  unlock(Metadata);
  return 0;
}

static int leaseForAddressLocal(md_backend *Backend, uintptr_t Address,
                                      uint32_t Owner, md_lease *Out) {
  if (Backend == nullptr || Out == nullptr || !md_contains(Backend, Address))
    return EINVAL;
  BackendImpl &Impl = Backend->Impl;
  uint32_t StartPage = static_cast<uint32_t>(
      (Address - reinterpret_cast<uintptr_t>(Impl.Payload)) / Impl.PageSize);
  SharedMetadata *Metadata = Impl.Metadata;
  lock(Metadata);
  for (uint32_t Index = 0; Index < Metadata->NextUnused; ++Index) {
    RunDescriptor &Run = Metadata->Runs[Index];
    if (Run.State.load(md_atomic::memory_order_acquire) == InUse &&
        Run.StartPage == StartPage && Run.Owner == Owner) {
      Out->block_id = Index;
      Out->owner = Owner;
      Out->generation = Run.Generation.load(md_atomic::memory_order_acquire);
      Out->address = Address;
      Out->size = static_cast<size_t>(Run.Pages) * Impl.PageSize;
      unlock(Metadata);
      return 0;
    }
  }
  unlock(Metadata);
  return ENOENT;
}

extern "C" int md_release_address(md_backend *Backend, uintptr_t Address,
                                    size_t Size, uint32_t Owner,
                                    int ClearPayload) {
  md_lease Lease{};
  int Result = md_lease_for_address(Backend, Address, Owner, &Lease);
  if (Result != 0 || Lease.size != Size)
    return Result != 0 ? Result : EINVAL;
  return md_release(Backend, &Lease, ClearPayload, nullptr);
}

extern "C" int md_contains(const md_backend *Backend, uintptr_t Address) {
  if (Backend == nullptr)
    return 0;
  uintptr_t Base = reinterpret_cast<uintptr_t>(Backend->Impl.Payload);
  return Address >= Base && Address < Base + Backend->Impl.Capacity * Backend->Impl.ArenaCount;
}

static void statsLocal(md_backend *Backend, md_stats *Out) {
  if (Backend == nullptr || Out == nullptr)
    return;
  SharedMetadata *Metadata = Backend->Impl.Metadata;
  memset(Out, 0, sizeof(*Out));
  lock(Metadata);
  drainLocked(Metadata);
  Out->acquire_calls = Metadata->AcquireCalls.load(md_atomic::memory_order_relaxed);
  Out->acquire_successes =
      Metadata->AcquireSuccesses.load(md_atomic::memory_order_relaxed);
  Out->release_calls = Metadata->ReleaseCalls.load(md_atomic::memory_order_relaxed);
  Out->release_successes =
      Metadata->ReleaseSuccesses.load(md_atomic::memory_order_relaxed);
  Out->split_count = Metadata->SplitCount.load(md_atomic::memory_order_relaxed);
  Out->merge_count = Metadata->MergeCount.load(md_atomic::memory_order_relaxed);
  Out->lock_acquisitions = Metadata->LockAcquisitions;
  Out->lock_contentions = Metadata->LockContentions;
  Out->lock_wait_ns = Metadata->LockWaitNs;
  Out->remote_drained = Metadata->RemoteDrained;
  Out->rseq_successes = Metadata->RseqSuccesses;
  Out->rseq_aborts = Metadata->RseqAborts;
  Out->descriptor_pops = Metadata->DescriptorPops;
  Out->address_steps = Metadata->AddressSteps;
  Out->hint_hits = Metadata->HintHits;
  queueLock(Metadata);
  Out->remote_enqueued = Metadata->RemoteEnqueued;
  Out->queue_contentions = Metadata->QueueContentions;
  Out->queue_wait_ns = Metadata->QueueWaitNs;
  pthread_mutex_unlock(&Metadata->QueueLock);
  for (uint32_t Index = 0; Index < Metadata->NextUnused; ++Index) {
    RunDescriptor &Run = Metadata->Runs[Index];
    uint64_t Bytes = static_cast<uint64_t>(Run.Pages) * Backend->Impl.PageSize;
    uint32_t StateValue = Run.State.load(md_atomic::memory_order_relaxed);
    if (StateValue == InUse || StateValue == Claiming || StateValue == Pending)
      Out->bytes_in_use += Bytes;
    else if (StateValue == Free)
      Out->bytes_free += Bytes;
  }
  unlock(Metadata);
}

extern "C" int md_current_arena(md_backend *Backend) {
  if (!Backend) return -1;
  if (Backend->Impl.ArenaCount == 1) return 0;
  int Cpu = sched_getcpu();
  return Cpu >= 0 && static_cast<uint32_t>(Cpu) < Backend->Impl.ArenaCount ? Cpu : -1;
}

extern "C" unsigned md_arena_count(md_backend *Backend) {
  return Backend ? Backend->Impl.ArenaCount : 0;
}

extern "C" int md_get_arena_layout(md_backend *Backend, unsigned Arena,
                                    md_arena_layout *Out) {
  if (!Backend || !Out || Arena >= Backend->Impl.ArenaCount)
    return EINVAL;
  Out->arena = Arena;
  Out->data_base = reinterpret_cast<uintptr_t>(Backend->Impl.Payload) +
                   static_cast<uintptr_t>(Arena) * Backend->Impl.Capacity;
  Out->data_size = Backend->Impl.Capacity;
  Out->nr_pages = static_cast<uint32_t>(Backend->Impl.Capacity /
                                        Backend->Impl.PageSize);
  return 0;
}

extern "C" int md_acquire_flags(md_backend *Backend, size_t Size, size_t Alignment,
                                 uint32_t Owner, unsigned Flags, md_lease *Out) {
  if (!Backend || !Out || !Size || Size > Backend->Impl.Capacity ||
      Alignment > Backend->Impl.Capacity ||
      (Alignment && (Alignment & (Alignment - 1))) || Owner == kNoOwner ||
      (Flags & ~static_cast<unsigned>(MD_ZERO))) return EINVAL;
  for (;;) {
    int Arena = md_current_arena(Backend);
    if (Arena < 0) return ENOTSUP;
    md_backend View = arenaView(Backend, static_cast<uint32_t>(Arena));
    SharedMetadata *Metadata = View.Impl.Metadata;
    int Fast = View.Impl.UseRseq && View.Impl.ArenaCount > 1
                 ? rseqLock(Metadata, static_cast<uint32_t>(Arena)) : -2;
    if (Fast == 0) {
      ++Metadata->LockAcquisitions;
      ++Metadata->RseqSuccesses;
    } else {
      lock(Metadata);
      if (Fast == -1) ++Metadata->RseqAborts;
      if (md_current_arena(Backend) != Arena) {
        unlock(Metadata);
        continue;
      }
    }
    View.Impl.Prelocked = true;
    int Result = acquireLocal(&View, Size, Alignment, Owner, Flags, Out);
    if (!Result) Out->block_id += static_cast<uint32_t>(Arena) * kMaxRuns;
    return Result;
  }
}

extern "C" int md_try_acquire(md_backend *Backend, md_candidate Candidate,
                               uint32_t Owner, md_lease *Out) {
  if (!Backend || !Out || Owner == kNoOwner ||
      Candidate.block_id >= Backend->Impl.ArenaCount * kMaxRuns) return EINVAL;
  uint32_t Arena = Candidate.block_id / kMaxRuns;
  if (md_current_arena(Backend) != static_cast<int>(Arena)) return EAGAIN;
  md_backend View = arenaView(Backend, Arena);
  Candidate.block_id %= kMaxRuns;
  int Result = tryAcquireLocal(&View, Candidate, Owner, Out);
  if (!Result) Out->block_id += Arena * kMaxRuns;
  return Result;
}

extern "C" int md_release(md_backend *Backend, const md_lease *Lease,
                           int ClearPayload, md_candidate *Candidate) {
  if (!Backend || !Lease || Lease->block_id >= Backend->Impl.ArenaCount * kMaxRuns)
    return EINVAL;
  uint32_t Arena = Lease->block_id / kMaxRuns;
  md_backend View = arenaView(Backend, Arena);
  md_lease Local = *Lease;
  Local.block_id %= kMaxRuns;
  if (md_current_arena(Backend) == static_cast<int>(Arena)) {
    int Result = releaseLocal(&View, &Local, ClearPayload, Candidate);
    if (!Result && Candidate) Candidate->block_id += Arena * kMaxRuns;
    return Result;
  }
  SharedMetadata *Metadata = View.Impl.Metadata;
  RunDescriptor &Run = Metadata->Runs[Local.block_id];
  uint64_t Expected = Local.generation;
  if (!Expected || !__atomic_compare_exchange_n(&Run.ReleaseTicket.Storage, &Expected, uint64_t(0),
                                    false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return ESTALE;
  if (Run.Owner != Local.owner || Run.Generation.load(__ATOMIC_RELAXED) != Local.generation ||
      Local.address != reinterpret_cast<uintptr_t>(View.Impl.Payload) +
                       static_cast<size_t>(Run.StartPage) * View.Impl.PageSize ||
      Local.size != static_cast<size_t>(Run.Pages) * View.Impl.PageSize) {
    Run.ReleaseTicket.store(Local.generation, __ATOMIC_RELEASE);
    return ESTALE;
  }
  bool Clear = ClearPayload || View.Impl.EagerClear;
  Run.State.store(Claiming, __ATOMIC_RELEASE);
  if (Clear) memset(reinterpret_cast<void *>(Local.address), 0, Local.size);
  Run.LastSession = Clear ? 0 : View.Impl.Session;
  Run.LastOwner = Clear ? kNoOwner : Local.owner;
  Run.QueueNext = kNoIndex;
  Run.State.store(Pending, __ATOMIC_RELEASE);
  // Intrusive messages live in descriptors, never in payload. The mailbox lock
  // is independent of the arena lock; producers never acquire the arena lock.
  queueLock(Metadata);
  if (Metadata->QueueTail == kNoIndex)
    Metadata->QueueHead.store(Local.block_id, __ATOMIC_RELEASE);
  else Metadata->Runs[Metadata->QueueTail].QueueNext = Local.block_id;
  Metadata->QueueTail = Local.block_id;
  ++Metadata->RemoteEnqueued;
  pthread_mutex_unlock(&Metadata->QueueLock);
  if (Candidate) *Candidate = {kNoIndex, 0};
  return 0;
}

extern "C" int md_drain_remote(md_backend *Backend, unsigned Arena) {
  if (!Backend || Arena >= Backend->Impl.ArenaCount) return EINVAL;
  SharedMetadata *Metadata = Backend->Impl.Metadata + Arena;
  lock(Metadata);
  drainLocked(Metadata);
  unlock(Metadata);
  return 0;
}

extern "C" int md_lease_for_address(md_backend *Backend, uintptr_t Address,
                                     uint32_t Owner, md_lease *Out) {
  if (!Backend || !Out || !md_contains(Backend, Address)) return EINVAL;
  uint32_t Arena = static_cast<uint32_t>(
      (Address - reinterpret_cast<uintptr_t>(Backend->Impl.Payload)) / Backend->Impl.Capacity);
  md_backend View = arenaView(Backend, Arena);
  int Result = leaseForAddressLocal(&View, Address, Owner, Out);
  if (!Result) Out->block_id += Arena * kMaxRuns;
  return Result;
}

extern "C" void md_get_stats(md_backend *Backend, md_stats *Out) {
  if (!Backend || !Out) return;
  memset(Out, 0, sizeof(*Out));
  for (uint32_t Arena = 0; Arena < Backend->Impl.ArenaCount; ++Arena) {
    md_backend View = arenaView(Backend, Arena);
    md_stats Stats{};
    statsLocal(&View, &Stats);
#define SUM(Field) Out->Field += Stats.Field
    SUM(acquire_calls); SUM(acquire_successes); SUM(release_calls);
    SUM(release_successes); SUM(split_count); SUM(merge_count);
    SUM(bytes_in_use); SUM(bytes_free); SUM(lock_acquisitions);
    SUM(lock_contentions); SUM(lock_wait_ns); SUM(remote_enqueued);
    SUM(remote_drained); SUM(rseq_successes); SUM(rseq_aborts);
    SUM(descriptor_pops); SUM(address_steps); SUM(hint_hits);
    SUM(queue_contentions); SUM(queue_wait_ns);
#undef SUM
  }
}
