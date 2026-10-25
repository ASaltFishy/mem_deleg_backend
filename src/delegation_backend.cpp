#include "include/latency_profile.h"
#include "delegation_backend.h"
#include "kernel_bridge.h"
#include "include/backend_broker_protocol.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/memfd.h>
#include "include/uapi/linux/memory_delegation.h"

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
#if defined(__GLIBC__) && __has_include(<sys/rseq.h>)
#define MD_HAS_LIBC_RSEQ 1
#include <sys/rseq.h>
#endif
#include <time.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <limits.h>

#ifdef MD_TEST_MIGRATION
extern "C" void md_test_metadata_locked();
#endif

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

// CPU ID only: no rseq critical section spans the blocking Arena Mutex.
struct alignas(32) CpuRseqArea {
  uint32_t CpuStart = UINT32_MAX;
  uint32_t CpuId = UINT32_MAX;
  uint64_t Cs = 0;
  uint32_t Flags = 0;
  uint32_t Reserved[3] = {};
};
static_assert(sizeof(CpuRseqArea) == 32, "original Linux rseq ABI");
struct CpuReader {
  CpuRseqArea Area;
  uint32_t *BorrowedId = nullptr;
  int Mode = 0; // 0 uninitialized, 1 owned, 2 syscall/initializing, 3 libc
};
static thread_local CpuReader CpuState;
static pthread_key_t CpuKey;
static pthread_once_t CpuKeyOnce = PTHREAD_ONCE_INIT;
static int CpuKeyError;
#if defined(__aarch64__)
constexpr uint32_t CpuRseqSignature = 0xd428bc00U;
#elif defined(__x86_64__)
constexpr uint32_t CpuRseqSignature = 0x53053053U;
#else
constexpr uint32_t CpuRseqSignature = 0;
#endif

static void releaseCpuReader(void *Pointer) {
  auto *Reader = static_cast<CpuReader *>(Pointer);
  int Previous = Reader->Mode;
  Reader->Mode = 2;
#if defined(SYS_rseq)
  if (Previous == 1)
    syscall(SYS_rseq, &Reader->Area, sizeof(Reader->Area), 1, CpuRseqSignature);
#else
  (void)Previous;
#endif
}
static void createCpuKey() { CpuKeyError = pthread_key_create(&CpuKey, releaseCpuReader); }
static inline int currentCpuFast();
static __attribute__((noinline, cold)) int initializeCpuReader() {
  // Allocation during libc/key initialization may recurse. Such calls use the
  // correct syscall path until this one-time registration is complete.
  CpuState.Mode = 2;
  const char *Force = getenv("MD_CPU_QUERY");
  if (Force && !strcmp(Force, "syscall")) return sched_getcpu();
#if defined(MD_HAS_LIBC_RSEQ)
  // Never replace or unregister a libc-owned rseq registration.
  if (__rseq_size >= 8) {
    auto *Area = static_cast<unsigned char *>(__builtin_thread_pointer()) + __rseq_offset;
    CpuState.BorrowedId = reinterpret_cast<uint32_t *>(Area + 4);
    CpuState.Mode = 3;
    return currentCpuFast();
  }
#endif
#if defined(SYS_rseq) && (defined(__aarch64__) || defined(__x86_64__))
  pthread_once(&CpuKeyOnce, createCpuKey);
  if (CpuKeyError) return sched_getcpu();
  if (syscall(SYS_rseq, &CpuState.Area, sizeof(CpuState.Area), 0,
              CpuRseqSignature) == 0) {
    // Unregister before pthread frees its TLS. Fork without CLONE_VM inherits
    // the registered address and the copied key/TLS, as specified by Linux.
    if (!pthread_setspecific(CpuKey, &CpuState)) {
      CpuState.Mode = 1;
      return currentCpuFast();
    }
    syscall(SYS_rseq, &CpuState.Area, sizeof(CpuState.Area), 1, CpuRseqSignature);
  }
#endif
  return sched_getcpu();
}
static inline int currentCpuFast() {
  int Mode = CpuState.Mode;
  if (__builtin_expect(Mode == 1, 1)) {
    int Cpu = static_cast<int>(__atomic_load_n(&CpuState.Area.CpuId, __ATOMIC_RELAXED));
    if (__builtin_expect(Cpu >= 0, 1)) return Cpu;
  } else if (Mode == 3) {
    int Cpu = static_cast<int>(__atomic_load_n(CpuState.BorrowedId, __ATOMIC_RELAXED));
    if (Cpu >= 0) return Cpu;
  } else if (Mode == 0) {
    return initializeCpuReader();
  }
  return sched_getcpu();
}

static thread_local bool BrokerCreating;

static int connectBroker(const md_config *Config, size_t PageSize,
                         uint32_t ArenaCount, int *MetadataFd, int *PayloadFd) {
  if (BrokerCreating) return ENOENT;
  const char *Socket = getenv("MD_BROKER_SOCKET");
#if defined(__ANDROID__)
  if (!Socket || !Socket[0]) Socket = "/data/local/tmp/md-backend.sock";
#else
  if (!Socket || !Socket[0]) return ENOENT;
#endif
  if (Config->reset) return EINVAL;
  sockaddr_un Address{};
  Address.sun_family = AF_UNIX;
  if (strlen(Socket) >= sizeof(Address.sun_path)) return ENAMETOOLONG;
  memcpy(Address.sun_path, Socket, strlen(Socket) + 1);
  int Fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (Fd < 0) return errno;
  if (connect(Fd, reinterpret_cast<sockaddr *>(&Address), sizeof(Address))) {
    int Error = errno == ENOENT ? ECONNREFUSED : errno; close(Fd); return Error;
  }
  md_broker_request Request{};
  Request.magic = MD_BROKER_MAGIC;
  Request.version = MD_BROKER_VERSION;
  Request.page_size = static_cast<uint32_t>(PageSize);
  Request.arena_count = ArenaCount;
  Request.capacity = Config->capacity;
  Request.fixed_address = Config->fixed_address;
  memcpy(Request.domain, Config->domain, strlen(Config->domain) + 1);
  if (send(Fd, &Request, sizeof(Request), MSG_NOSIGNAL) != sizeof(Request)) {
    int Error = errno ? errno : EIO; close(Fd); return Error;
  }
  md_broker_response Response{};
  iovec Iov{&Response, sizeof(Response)};
  alignas(cmsghdr) unsigned char Control[CMSG_SPACE(2 * sizeof(int))]{};
  msghdr Message{}; Message.msg_iov = &Iov; Message.msg_iovlen = 1;
  Message.msg_control = Control; Message.msg_controllen = sizeof(Control);
  ssize_t Count = recvmsg(Fd, &Message, MSG_CMSG_CLOEXEC);
  close(Fd);
  int Received[2]{-1, -1};
  cmsghdr *Header = CMSG_FIRSTHDR(&Message);
  if (Header && Header->cmsg_level == SOL_SOCKET && Header->cmsg_type == SCM_RIGHTS &&
      Header->cmsg_len == CMSG_LEN(sizeof(Received))) memcpy(Received, CMSG_DATA(Header), sizeof(Received));
  int Error = 0;
  if (Count != sizeof(Response) || (Message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
      Response.magic != MD_BROKER_MAGIC || Response.version != MD_BROKER_VERSION) Error = EPROTO;
  else if (Response.error) Error = Response.error;
  else if (Received[0] < 0 || Received[1] < 0) Error = EPROTO;
  if (Error) {
    if (Received[0] >= 0) close(Received[0]);
    if (Received[1] >= 0) close(Received[1]);
    return Error;
  }
  *MetadataFd = Received[0]; *PayloadFd = Received[1];
  return 0;
}

static int openSharedObject(const char *Name, int Flags, mode_t Mode) {
#if defined(__ANDROID__) || defined(MD_BROKER_MEMFD)
  (void)Flags; (void)Mode;
  if (!BrokerCreating) { errno = EPERM; return -1; }
  return static_cast<int>(syscall(SYS_memfd_create, Name, MFD_CLOEXEC | MFD_ALLOW_SEALING));
#else
  return shm_open(Name, Flags, Mode);
#endif
}

constexpr uint64_t kMagic = 0x4d4442454e443031ULL;
constexpr uint32_t kVersion = 8;
constexpr size_t kKernelLogBytes = 1U << 20;
constexpr size_t kMaxKernelPageSize = 65536;
constexpr uint32_t kNoIndex = UINT32_MAX;
constexpr uint32_t kNoOwner = UINT32_MAX;
constexpr uint32_t kMaxRuns = 8192;
constexpr uint32_t kNumBins = 24;

enum RunState : uint32_t { Unused, Free, Claiming, InUse, Pending, Quarantined };

// Every descriptor read/write is serialized by ArenaMutex. Relaxed atomics
// retain the representation; publication to other allocators is the Mutex unlock.
// Shared journal tail/ready/completion atomics keep their acquire/release ordering.
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
  uint64_t AllocationId;
  uint64_t KernelSession;
  uint64_t HolderSession;
};

struct alignas(kMaxKernelPageSize) SharedMetadata {
  uint64_t Magic;
  uint32_t Version;
  uint32_t PageSize;
  uint64_t Capacity;
  uint64_t ArenaStride;
  uintptr_t FixedAddress;
  pthread_mutex_t ArenaMutex;
  uint32_t KernelEnabled; // 0 undecided, 1 real kernel, 2 userspace only
  int32_t KernelError;
  uint64_t NextAllocation;
  uint64_t KernelRejected;
  md_shared_slot ErrorReceipts[16];
  uint32_t DescriptorHead;
  uint32_t NextUnused;
  uint32_t InsertHint;
  uint32_t ArenaId;
  uint32_t ArenaCount;
  uint32_t BumpPage;
  uint32_t FreeHead;
  uint32_t PendingHead; // migrated normal FREE only; linked through Run.Next
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
  // One shared pair for all participants, initialized only by domain creator.
  alignas(kMaxKernelPageSize) unsigned char KernelLog[kKernelLogBytes];
};

// Operation-local view: lifecycle state and the private initialization Mutex
// never need to be copied into malloc/free stack frames.
struct ArenaImpl {
  SharedMetadata *Metadata = nullptr;
  unsigned char *Payload = nullptr;
  size_t Capacity = 0;
  size_t PageSize = 0;
  uint64_t Session = 0;
  uint32_t ArenaCount = 1;
  uint32_t PageShift = 0;
  md_kernel_bridge *KernelBridge = nullptr;
};
struct BackendImpl : ArenaImpl {
  int MetadataFd = -1;
  int PayloadFd = -1;
  size_t ArenaStride = 0;
  size_t WindowSize = 0;
  bool UseRseq = true;
  size_t MetadataSize = 0;
  md_kernel_bridge **KernelBridges = nullptr;
  size_t KernelBridgesSize = 0;
  uint32_t KernelPid = 0;
  pthread_mutex_t BridgeInitMutex;
  int KernelMode = MD_KERNEL_BRIDGE_OPTIONAL;
  bool KernelDisabled = false;
};

static uint32_t sizeClass(uint32_t Pages) {
  uint32_t Value = Pages > 0 ? Pages - 1 : 0;
  uint32_t Class = Value ? 32U - static_cast<uint32_t>(__builtin_clz(Value)) : 0;
  return Class < kNumBins ? Class : kNumBins - 1;
}

static uint64_t nowNs() {
  timespec Time;
  clock_gettime(CLOCK_MONOTONIC, &Time);
  return uint64_t(Time.tv_sec) * 1000000000 + uint64_t(Time.tv_nsec);
}

static void lock(SharedMetadata *Metadata) {
  if (pthread_mutex_trylock(&Metadata->ArenaMutex) == 0) {
    MD_STAT(++Metadata->LockAcquisitions);
    return;
  }
#if !MD_PERF_MINIMAL
  uint64_t Start = nowNs();
#endif
  int Error = pthread_mutex_lock(&Metadata->ArenaMutex);
  if (Error) abort();
  MD_STAT(++Metadata->LockAcquisitions);
  MD_STAT(++Metadata->LockContentions);
  MD_STAT(Metadata->LockWaitNs += nowNs() - Start);
}

static void unlock(SharedMetadata *Metadata) {
  if (pthread_mutex_unlock(&Metadata->ArenaMutex)) abort();
}

static md_shared_log *sharedLog(SharedMetadata *Metadata) {
  return reinterpret_cast<md_shared_log *>(Metadata->KernelLog);
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
  MD_STAT(++Metadata->DescriptorPops);
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
    MD_STAT(++Metadata->HintHits);
  }
  while (Next != kNoIndex &&
         Metadata->Runs[Next].StartPage < Run.StartPage) {
    Prev = Next;
    Next = Metadata->Runs[Next].Next;
    MD_STAT(++Metadata->AddressSteps);
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
  Run.State.store(StateValue, md_atomic::memory_order_relaxed);
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
  Run.State.store(Unused, md_atomic::memory_order_relaxed);
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
      MD_STAT(Metadata->MergeCount.incrementLocked());
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
      MD_STAT(Metadata->MergeCount.incrementLocked());
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

// Both claim paths hold ArenaMutex and have validated a Free candidate.
// Preserve the lease/generation transition without rewriting an unchanged range.
static void assignClaimedLease(ArenaImpl *Backend, uint32_t Index,
                                uint32_t Owner, md_lease *Out) {
  RunDescriptor &Run = Backend->Metadata->Runs[Index];
  Run.Owner = Owner;
  uint64_t Generation = Run.Generation.incrementLocked() + 1;
  Run.State.store(Claiming, md_atomic::memory_order_relaxed);
  Out->block_id = Index;
  Out->owner = Owner;
  Out->generation = Generation;
  Out->address = reinterpret_cast<uintptr_t>(Backend->Payload) +
                 static_cast<uintptr_t>(Run.StartPage) * Backend->PageSize;
  Out->size = static_cast<size_t>(Run.Pages) * Backend->PageSize;
}

// Exact size/alignment match: no prefix/suffix descriptors or provenance copies.
// This still removes the run from BOTH shared free indexes before claiming it.
static void claimWholeRun(ArenaImpl *Backend, uint32_t Index,
                           uint32_t Owner, md_lease *Out) {
  unlinkBin(Backend->Metadata, Index);
  unlinkAddress(Backend->Metadata, Index);
  assignClaimedLease(Backend, Index, Owner, Out);
}

static __attribute__((noinline)) int claimRun(ArenaImpl *Backend, uint32_t Index, uint32_t StartPage,
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
    MD_STAT(Metadata->SplitCount.incrementLocked());
  }
  if (Suffix != kNoIndex) {
    makeFree(Metadata, Suffix, SuffixStart, SuffixPages);
    Metadata->Runs[Suffix].LastSession = LastSession;
    Metadata->Runs[Suffix].LastOwner = LastOwner;
    MD_STAT(Metadata->SplitCount.incrementLocked());
  }
  Run.StartPage = StartPage;
  Run.Pages = NeededPages;
  assignClaimedLease(Backend, Index, Owner, Out);
  return 0;
}

// Failure recovery is shared by exact-fit, split, and bump allocations. Keep
// its merge/list repair code out of the successful commit's register live set.
static __attribute__((noinline, cold)) int rollbackAcquireLocked(
    ArenaImpl &Impl, md_lease &Lease, int Error) {
  SharedMetadata *Metadata = Impl.Metadata;
  RunDescriptor &Run = Metadata->Runs[Lease.block_id];
  Run.Owner = kNoOwner;
  Run.Generation.incrementLocked();
  Run.State.store(Free, __ATOMIC_RELAXED);
  Run.Prev = Run.Next = Run.BinPrev = Run.BinNext = kNoIndex;
  linkAddress(Metadata, Lease.block_id);
  linkBin(Metadata, Lease.block_id);
  mergeFreeRun(Metadata, Lease.block_id);
  MD_STAT(Metadata->AcquireSuccesses.store(Metadata->AcquireSuccesses.load(__ATOMIC_RELAXED) - 1, __ATOMIC_RELAXED));
  unlock(Metadata);
  return Error;
}

static __attribute__((always_inline)) inline int finishAcquireLocked(
    ArenaImpl &Impl, md_lease &Lease, unsigned Flags) {
  MD_SCOPE(6, FinishProbe);
  SharedMetadata *Metadata = Impl.Metadata;
  RunDescriptor &Run = Metadata->Runs[Lease.block_id];
  int Error = Metadata->KernelError;
  if (!Error && Metadata->NextAllocation == UINT64_MAX) Error = EOVERFLOW;
  if (!Error) {
    Run.AllocationId = ++Metadata->NextAllocation;
    Run.KernelSession = md_kernel_bridge_session(Impl.KernelBridge);
    Run.HolderSession = Impl.Session;
    Lease.allocation_id = Run.AllocationId;
    Lease.kernel_session = Run.KernelSession;
    if (Impl.KernelBridge) {
      Error = md_kernel_bridge_alloc(Impl.KernelBridge, Run.KernelSession,
          static_cast<uint16_t>(Metadata->ArenaId),
          uint64_t(Metadata->ArenaId) * kMaxRuns + Lease.block_id + 1,
          Lease.generation, Run.AllocationId, Run.StartPage, Run.Pages);
    }
  }
  if (Error) return rollbackAcquireLocked(Impl, Lease, Error);
  // Performance experiment: no implicit handoff/provenance clearing.
  // Explicit zero-allocation requests retain their API semantics.
  if (Flags & MD_ZERO) memset(reinterpret_cast<void *>(Lease.address), 0, Lease.size);
  Run.State.store(InUse, __ATOMIC_RELAXED);
  Run.ReleaseTicket.store(Lease.generation, __ATOMIC_RELAXED);
  unlock(Metadata);
  return 0;
}

} // namespace

struct md_backend { BackendImpl Impl; };

struct md_arena_view { ArenaImpl Impl; };
static md_arena_view arenaView(md_backend *Backend, uint32_t Arena) {
  const BackendImpl &Root = Backend->Impl;
  ArenaImpl View = static_cast<const ArenaImpl &>(Root);
  View.Metadata += Arena;
  View.Payload += static_cast<size_t>(Arena) * Root.ArenaStride;
  View.KernelBridge = Root.KernelBridges ? Root.KernelBridges[Arena] : nullptr;
  return {View};
}

static int openKernelBridge(md_backend *Backend, uint32_t Arena) {
  SharedMetadata *Metadata = Backend->Impl.Metadata + Arena;
  md_kernel_bridge_config Config{};
  Config.arena_cpu = Arena;
  Config.arena_data_base = reinterpret_cast<uintptr_t>(Backend->Impl.Payload) +
      static_cast<uintptr_t>(Arena) * Backend->Impl.ArenaStride;
  Config.arena_nr_pages = static_cast<uint32_t>(Backend->Impl.Capacity / Backend->Impl.PageSize);
  Config.mode = Backend->Impl.KernelMode;
  Config.ring_memory = Metadata->KernelLog;
  Config.ring_bytes = sizeof(Metadata->KernelLog);
  return md_kernel_bridge_open(&Backend->Impl.KernelBridges[Arena], &Config);
}

static __attribute__((noinline, cold)) int refreshKernelBridgesAfterForkSlow(
    md_backend *Backend, uint32_t Pid) {
  int LockError = pthread_mutex_lock(&Backend->Impl.BridgeInitMutex);
  if (LockError) return LockError;
  if (__atomic_load_n(&Backend->Impl.KernelPid, __ATOMIC_RELAXED) == Pid) {
    pthread_mutex_unlock(&Backend->Impl.BridgeInitMutex);
    return 0;
  }

  for (uint32_t Arena = 0; Arena < Backend->Impl.ArenaCount; ++Arena) {
    md_kernel_bridge_close(Backend->Impl.KernelBridges[Arena]);
    Backend->Impl.KernelBridges[Arena] = nullptr;
  }
  for (uint32_t Arena = 0; Arena < Backend->Impl.ArenaCount; ++Arena) {
    int Error = openKernelBridge(Backend, Arena);
    if (Error) { pthread_mutex_unlock(&Backend->Impl.BridgeInitMutex); return Error; }
  }
  __atomic_store_n(&Backend->Impl.KernelPid, Pid, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&Backend->Impl.BridgeInitMutex);
  return 0;
}

static __attribute__((always_inline)) inline int refreshKernelBridgesAfterFork(
    md_backend *Backend) {
  if (!Backend || Backend->Impl.KernelDisabled) return 0;
  uint32_t Pid = md_process_id();
  if (__builtin_expect(
          __atomic_load_n(&Backend->Impl.KernelPid, __ATOMIC_ACQUIRE) == Pid, 1))
    return 0;
  return refreshKernelBridgesAfterForkSlow(Backend, Pid);
}

static uint32_t publishLocked(SharedMetadata *Metadata, uint32_t Index) {
  RunDescriptor &Run = Metadata->Runs[Index];
  Run.Owner = kNoOwner;
  Run.Generation.incrementLocked();
  Run.State.store(Free, __ATOMIC_RELAXED);
  Run.Prev = Run.Next = Run.BinPrev = Run.BinNext = kNoIndex;
  linkAddress(Metadata, Index);
  linkBin(Metadata, Index);
  return mergeFreeRun(Metadata, Index);
}

static bool recordMatches(SharedMetadata *Metadata, const md_shared_record &Record,
                           uint32_t *Index) {
  uint64_t First = uint64_t(Metadata->ArenaId) * kMaxRuns + 1;
  if (Record.block_id < First || Record.block_id - First >= Metadata->NextUnused)
    return false;
  *Index = static_cast<uint32_t>(Record.block_id - First);
  RunDescriptor &Run = Metadata->Runs[*Index];
  return (Run.State.load(__ATOMIC_RELAXED) == InUse ||
          Run.State.load(__ATOMIC_RELAXED) == Pending) &&
      Run.Generation.load(__ATOMIC_RELAXED) == Record.generation &&
      Run.AllocationId == Record.allocation_id && Run.KernelSession == Record.session &&
      Run.StartPage == Record.start_page && Run.Pages == Record.nr_pages &&
      Record.cpu == Metadata->ArenaId;
}

static void drainLocked(SharedMetadata *Metadata, bool OwnerValidated = false) {
  if (!OwnerValidated && (Metadata->ArenaCount > 1 || Metadata->KernelEnabled == 1) &&
      currentCpuFast() != static_cast<int>(Metadata->ArenaId)) return;
  md_shared_log *Log = sharedLog(Metadata);
  if (Metadata->KernelError) return;
  if (__atomic_load_n(&Log->state, __ATOMIC_ACQUIRE) != MD_SHARED_LOG_RUNNING) {
    // The kernel preserves the first fatal error outside the reusable slots.
    int Error = Log->normal_error.result;
    if (Error) Metadata->ErrorReceipts[Metadata->KernelRejected++ % 16] = Log->normal_error;
    Metadata->KernelError = Error < 0 && Error >= -4095 ? -Error : ESHUTDOWN;
    return;
  }

  // No ordinary success receipt scan. Only migrated normal FREEs need an
  // action after kernel success. Their descriptors cannot split/merge/reuse
  // while Pending, so a contiguous successful head confirms the saved ticket.
  if (Metadata->PendingHead != kNoIndex) {
    uint64_t Head = __atomic_load_n(&Log->normal.head, __ATOMIC_ACQUIRE);
    uint64_t Tail = __atomic_load_n(&Log->normal.reserved, __ATOMIC_RELAXED);
    if (Head > Tail) { Metadata->KernelError = EPROTO; return; }
    uint32_t *Link = &Metadata->PendingHead;
    uint32_t Seen = 0;
    while (*Link != kNoIndex) {
      uint32_t Index = *Link;
      if (Index >= Metadata->NextUnused || ++Seen > kMaxRuns) {
        Metadata->KernelError = EPROTO; return;
      }
      RunDescriptor &Run = Metadata->Runs[Index];
      uint64_t Ticket = Run.ReleaseTicket.load(__ATOMIC_RELAXED);
      if (Run.State.load(__ATOMIC_RELAXED) != Pending || !Ticket || Ticket > Tail ||
          !Run.Generation.load(__ATOMIC_RELAXED) || !Run.AllocationId ||
          !Run.KernelSession || Run.Owner == kNoOwner || !Run.Pages) {
        Metadata->KernelError = EPROTO; return;
      }
      if (Ticket > Head) { Link = &Run.Next; continue; }
      *Link = Run.Next;
      Run.ReleaseTicket.store(0, __ATOMIC_RELAXED);
      publishLocked(Metadata, Index);
      MD_STAT(Metadata->ReleaseSuccesses.incrementLocked());
    }
  }

  md_shared_queue &Queue = Log->remote;
  uint64_t Cursor = __atomic_load_n(&Queue.reclaimed, __ATOMIC_RELAXED);
  uint64_t Completed = __atomic_load_n(&Queue.completed, __ATOMIC_ACQUIRE);
  if (Metadata->KernelEnabled == 2) {
    uint64_t Reserved = __atomic_load_n(&Queue.reserved, __ATOMIC_ACQUIRE);
    Completed = Cursor;
    while (Completed != Reserved && Completed - Cursor < Log->capacity) {
      md_shared_slot &Slot = Log->slots[Log->capacity + Completed % Log->capacity];
      if (__atomic_load_n(&Slot.ready, __ATOMIC_ACQUIRE) != Completed + 1) break;
      ++Completed;
    }
  }
  if (Completed < Cursor || Completed - Cursor > Log->capacity) {
    Metadata->KernelError = EPROTO; return;
  }
  if (Cursor == Completed) return;
  for (; Cursor != Completed; ++Cursor) {
    md_shared_slot &Slot = Log->slots[Log->capacity + Cursor % Log->capacity];
    if (__atomic_load_n(&Slot.ready, __ATOMIC_ACQUIRE) != Cursor + 1 ||
        (Slot.reserved & ~static_cast<unsigned>(MD_USER_FREE_CLEAR))) {
      Metadata->KernelError = EPROTO; return;
    }
    uint32_t Index = 0;
    bool Matches = recordMatches(Metadata, Slot.record, &Index);
    if (Metadata->KernelEnabled == 2) Slot.result = Matches ? 0 : -ESTALE;
    if (Slot.result) {
      Metadata->ErrorReceipts[Metadata->KernelRejected++ % 16] = Slot;
      if (Matches) {
        Metadata->Runs[Index].State.store(Quarantined, __ATOMIC_RELEASE);
        Metadata->Runs[Index].ReleaseTicket.store(0, __ATOMIC_RELEASE);
      }
    } else {
      if (!Matches || Slot.record.op != MD_LOG_FREE) {
        Metadata->KernelError = EPROTO; return;
      }
      RunDescriptor &Run = Metadata->Runs[Index];
      Run.LastSession = Run.HolderSession;
      Run.LastOwner = Run.Owner;
      if (Slot.reserved & MD_USER_FREE_CLEAR) {
        uintptr_t Address = Metadata->FixedAddress + size_t(Metadata->ArenaId) * Metadata->ArenaStride +
                            size_t(Run.StartPage) * Metadata->PageSize;
        memset(reinterpret_cast<void *>(Address), 0, size_t(Run.Pages) * Metadata->PageSize);
        Run.LastSession = 0;
        Run.LastOwner = kNoOwner;
      }
      Run.ReleaseTicket.store(0, __ATOMIC_RELAXED);
      publishLocked(Metadata, Index);
      MD_STAT(++Metadata->RemoteDrained);
      MD_STAT(Metadata->ReleaseCalls.incrementLocked());
      MD_STAT(Metadata->ReleaseSuccesses.incrementLocked());
    }
  }
  if (Metadata->KernelEnabled == 2)
    __atomic_store_n(&Queue.completed, Cursor, __ATOMIC_RELEASE);
  __atomic_store_n(&Queue.reclaimed, Cursor, __ATOMIC_RELEASE);
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
  if (!PageSize || (PageSize & (PageSize - 1))) return EINVAL;
  size_t Capacity = (Config->capacity / PageSize) * PageSize;
  long CpuCount = sysconf(_SC_NPROCESSORS_CONF);
  if (CpuCount <= 0 || CpuCount > 1024) return ENOTSUP;
  // Single-arena mode is only a contention-control experiment, not the default.
  const char *Single = getenv("MD_SINGLE_ARENA");
  uint32_t ArenaCount = Single && Single[0] == '1' ? 1 : static_cast<uint32_t>(CpuCount);
  size_t MetadataSize = sizeof(SharedMetadata) * ArenaCount;
  if (Capacity > SIZE_MAX - PageSize) return EINVAL;
  size_t ArenaStride = Capacity + PageSize;
  if (ArenaCount > 1 && ArenaStride >
      (SIZE_MAX - Capacity) / (ArenaCount - 1)) return EINVAL;
  size_t WindowSize = ArenaStride * (ArenaCount - 1) + Capacity;
  if (Capacity > SIZE_MAX / ArenaCount) return EINVAL;
  size_t PayloadSize = Capacity * ArenaCount;
  if (Config->fixed_address > UINTPTR_MAX - WindowSize) return EINVAL;
  if (strlen(Config->domain) > 120 || strchr(Config->domain, '/') ||
      Capacity == 0 || Capacity / PageSize > UINT32_MAX ||
      Config->fixed_address % Capacity != 0)
    return EINVAL;
  char MetadataName[160], PayloadName[160];
  snprintf(MetadataName, sizeof(MetadataName), "/%s_meta", Config->domain);
  snprintf(PayloadName, sizeof(PayloadName), "/%s_data", Config->domain);
  int MetadataFd = -1, BrokerPayloadFd = -1;
  int BrokerResult = connectBroker(Config, PageSize, ArenaCount, &MetadataFd, &BrokerPayloadFd);
  bool FromBroker = BrokerResult == 0;
  if (BrokerResult && BrokerResult != ENOENT) return BrokerResult;
#if defined(__ANDROID__)
  if (!FromBroker && !BrokerCreating) return ENOTCONN;
#endif
  bool Created = false;
  if (!FromBroker) {
    MetadataFd = openSharedObject(MetadataName, O_RDWR | O_CREAT | O_EXCL, 0600);
    Created = MetadataFd >= 0;
    if (!Created && errno == EEXIST) MetadataFd = openSharedObject(MetadataName, O_RDWR, 0600);
  }
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
  int PayloadFd = BrokerPayloadFd;
  SharedMetadata *Metadata = nullptr;
  void *Payload = MAP_FAILED;
  void *Storage = MAP_FAILED;
  md_kernel_bridge **KernelBridges = nullptr;
  size_t KernelBridgesSize = static_cast<size_t>(ArenaCount) *
                             sizeof(*KernelBridges);
  auto Fail = [&](int Error) {
    if (Storage != MAP_FAILED) munmap(Storage, sizeof(md_backend));
    if (KernelBridges) {
      for (uint32_t Arena = 0; Arena < ArenaCount; ++Arena)
        md_kernel_bridge_close(KernelBridges[Arena]);
      munmap(KernelBridges, KernelBridgesSize);
    }
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
  if (FromBroker && Initialize) return Fail(EPROTO);
  if (!Initialize && (Config->reset ||
                      MetaStat.st_size != static_cast<off_t>(MetadataSize)))
    return Fail(EINVAL);
  if (!FromBroker) PayloadFd = openSharedObject(PayloadName,
                               O_RDWR | (Initialize ? O_CREAT : 0), 0600);
  if (PayloadFd < 0) return Fail(errno);
  if (Initialize) {
    if (ftruncate(PayloadFd, static_cast<off_t>(PayloadSize)) != 0 ||
        ftruncate(MetadataFd, static_cast<off_t>(MetadataSize)) != 0)
      return Fail(errno);
  } else {
    if (fstat(PayloadFd, &DataStat) != 0) return Fail(errno);
    if (DataStat.st_size != static_cast<off_t>(PayloadSize)) return Fail(EINVAL);
  }
  // SharedMetadata contains page-aligned rings on all supported page sizes.
  void *Reservation = mmap(nullptr, MetadataSize + kMaxKernelPageSize,
                            PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Reservation == MAP_FAILED) return Fail(errno);
  uintptr_t Raw = reinterpret_cast<uintptr_t>(Reservation);
  uintptr_t Aligned = (Raw + kMaxKernelPageSize - 1) &
                      ~(uintptr_t(kMaxKernelPageSize) - 1);
  size_t Prefix = Aligned - Raw;
  size_t Suffix = kMaxKernelPageSize - Prefix;
  if (Prefix) munmap(Reservation, Prefix);
  if (Suffix) munmap(reinterpret_cast<void *>(Aligned + MetadataSize), Suffix);
  Metadata = static_cast<SharedMetadata *>(
      mmap(reinterpret_cast<void *>(Aligned), MetadataSize,
           PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, MetadataFd, 0));
  if (Metadata == MAP_FAILED) {
    int Error = errno;
    munmap(reinterpret_cast<void *>(Aligned), MetadataSize);
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
      Current->FixedAddress = Config->fixed_address;
      Current->ArenaStride = ArenaStride;
      Current->Capacity = Capacity;
      Current->PageSize = static_cast<uint32_t>(PageSize);
      Current->FreeHead = Current->InsertHint = Current->PendingHead = kNoIndex;
      Current->DescriptorHead = kNoIndex;
      Error = pthread_mutex_init(&Current->ArenaMutex, &Attributes);
      if (Error) {
        pthread_mutexattr_destroy(&Attributes);
        return Fail(Error);
      }
      Error = md_kernel_log_initialize(Current->KernelLog, sizeof(Current->KernelLog),
          static_cast<uint32_t>(Capacity / PageSize));
      if (Error) { pthread_mutexattr_destroy(&Attributes); return Fail(Error); }
      for (uint32_t &Head : Current->BinHeads) Head = kNoIndex;
    }
    pthread_mutexattr_destroy(&Attributes);
    Metadata->Version = kVersion;
    Metadata->PageSize = static_cast<uint32_t>(PageSize);
    Metadata->Capacity = Capacity;
    Metadata->ArenaStride = ArenaStride;
    Metadata->FixedAddress = Config->fixed_address;
    md_atomic::atomic_thread_fence(md_atomic::memory_order_release);
    Metadata->Magic = kMagic;
  } else if (Metadata->Magic != kMagic || Metadata->Version != kVersion ||
             Metadata->Capacity != Capacity ||
             Metadata->ArenaStride != ArenaStride ||
             Metadata->FixedAddress != Config->fixed_address ||
             Metadata->PageSize != PageSize || Metadata->ArenaCount != ArenaCount) {
    return Fail(EINVAL);
  }
  for (uint32_t Arena = 0; Arena < ArenaCount; ++Arena) {
    SharedMetadata *Current = Metadata + Arena;
    if (Current->FixedAddress != Config->fixed_address || Current->ArenaStride != ArenaStride ||
        Current->PageSize != PageSize || Current->Capacity != Capacity) return Fail(EINVAL);
  }
  Payload = mmap(reinterpret_cast<void *>(Config->fixed_address), WindowSize,
                 PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                 -1, 0);
  if (Payload == MAP_FAILED) return Fail(errno);
  for (uint32_t Arena = 0; Arena < ArenaCount; ++Arena) {
    void *ArenaAddress = static_cast<unsigned char *>(Payload) +
                         static_cast<size_t>(Arena) * ArenaStride;
    off_t ArenaOffset = static_cast<off_t>(Arena) *
                        static_cast<off_t>(Capacity);
    void *ArenaMapping = mmap(ArenaAddress, Capacity, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_FIXED, PayloadFd, ArenaOffset);
    if (ArenaMapping == MAP_FAILED) return Fail(errno);
  }
  uint64_t Session = ++Metadata->NextSession;
  Storage = mmap(nullptr, sizeof(md_backend), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Storage == MAP_FAILED) return Fail(errno);
  auto *Backend = new (Storage) md_backend;
  int PrivateMutexError = pthread_mutex_init(&Backend->Impl.BridgeInitMutex, nullptr);
  if (PrivateMutexError) return Fail(PrivateMutexError);
  Backend->Impl.MetadataFd = MetadataFd;
  Backend->Impl.PayloadFd = PayloadFd;
  Backend->Impl.Metadata = Metadata;
  Backend->Impl.Payload = static_cast<unsigned char *>(Payload);
  Backend->Impl.Capacity = Capacity;
  Backend->Impl.ArenaStride = ArenaStride;
  Backend->Impl.WindowSize = WindowSize;
  Backend->Impl.PageSize = PageSize;
  Backend->Impl.PageShift = static_cast<uint32_t>(__builtin_ctzl(PageSize));
  Backend->Impl.Session = Session;
  Backend->Impl.MetadataSize = MetadataSize;
  Backend->Impl.ArenaCount = ArenaCount;
  KernelBridges = static_cast<md_kernel_bridge **>(
      mmap(nullptr, KernelBridgesSize, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (KernelBridges == MAP_FAILED) {
    KernelBridges = nullptr;
    return Fail(errno);
  }
  Backend->Impl.KernelBridges = KernelBridges;
  Backend->Impl.KernelBridgesSize = KernelBridgesSize;
  const char *KernelMode = getenv("MD_KERNEL_BRIDGE");
  const bool KernelDisabled = KernelMode &&
      (!strcmp(KernelMode, "0") || !strcmp(KernelMode, "off"));
  const int BridgeMode = KernelMode &&
      (!strcmp(KernelMode, "1") || !strcmp(KernelMode, "required"))
      ? MD_KERNEL_BRIDGE_REQUIRED : MD_KERNEL_BRIDGE_OPTIONAL;
  Backend->Impl.KernelDisabled = KernelDisabled;
  Backend->Impl.KernelMode = BridgeMode;
  Backend->Impl.KernelPid = static_cast<uint32_t>(getpid());
  if (!KernelDisabled) {
    for (uint32_t Arena = 0; Arena < ArenaCount; ++Arena) {
      int Error = openKernelBridge(Backend, Arena);
      if (Error) return Fail(Error);
    }
  }
  uint32_t Policy = Backend->Impl.KernelBridges[0] ? 1 : 2;
  for (uint32_t Arena = 0; Arena < ArenaCount; ++Arena) {
    SharedMetadata *Current = Metadata + Arena;
    if ((Backend->Impl.KernelBridges[Arena] ? 1U : 2U) != Policy ||
        (Current->KernelEnabled && Current->KernelEnabled != Policy))
      return Fail(EPROTO);
  }
  for (uint32_t Arena = 0; Arena < ArenaCount; ++Arena)
    (Metadata + Arena)->KernelEnabled = Policy;
  flock(MetadataFd, LOCK_UN);
  const char *UseRseq = getenv("MD_RSEQ");
  Backend->Impl.UseRseq = !UseRseq || UseRseq[0] != '0';
  int Error = pthread_atfork(nullptr, nullptr, afterForkChild);
  if (Error) {
    Backend->~md_backend();
    return Fail(Error);
  }
  *Out = Backend;
  return 0;
}

extern "C" int md_backend_checkpoint(md_backend *Backend) {
  if (!Backend) return EINVAL;
  int Error = refreshKernelBridgesAfterFork(Backend);
  if (Error) return Error;
  cpu_set_t Saved;
  if (sched_getaffinity(0, sizeof(Saved), &Saved)) return errno;
  int Result = 0;
  for (uint32_t Arena = 0; Arena < Backend->Impl.ArenaCount && !Result; ++Arena) {
    SharedMetadata *Metadata = Backend->Impl.Metadata + Arena;
    md_shared_log *Log = sharedLog(Metadata);
    uint64_t Normal = __atomic_load_n(&Log->normal.reserved, __ATOMIC_ACQUIRE);
    uint64_t Remote = __atomic_load_n(&Log->remote.reserved, __ATOMIC_ACQUIRE);
    cpu_set_t One; CPU_ZERO(&One); CPU_SET(Arena, &One);
    if (sched_setaffinity(0, sizeof(One), &One)) {
      if (__atomic_load_n(&Log->normal.head, __ATOMIC_ACQUIRE) != Normal ||
          __atomic_load_n(&Log->remote.reclaimed, __ATOMIC_ACQUIRE) != Remote) Result = EAGAIN;
      continue;
    }
    uint64_t Deadline = nowNs() + 1000000000ULL;
    for (;;) {
      lock(Metadata);
      drainLocked(Metadata);
      Result = Metadata->KernelError;
      bool Done = __atomic_load_n(&Log->normal.head, __ATOMIC_ACQUIRE) >= Normal &&
                  __atomic_load_n(&Log->remote.reclaimed, __ATOMIC_ACQUIRE) >= Remote &&
                  Metadata->PendingHead == kNoIndex;
      unlock(Metadata);
      if (Result || Done) break;
      if (nowNs() >= Deadline) { Result = ETIMEDOUT; break; }
      usleep(100); // owner CPU scheduling consumes; never an explicit DRAIN.
    }
  }
  if (sched_setaffinity(0, sizeof(Saved), &Saved) && !Result) Result = errno;
  return Result;
}

extern "C" void md_backend_close(md_backend *Backend) {
  if (Backend == nullptr)
    return;
  if (Backend->Impl.KernelBridges) {
    for (uint32_t Arena = 0; Arena < Backend->Impl.ArenaCount; ++Arena)
      md_kernel_bridge_close(Backend->Impl.KernelBridges[Arena]);
    munmap(Backend->Impl.KernelBridges,
           Backend->Impl.KernelBridgesSize);
  }
  munmap(Backend->Impl.Payload, Backend->Impl.WindowSize);
  munmap(Backend->Impl.Metadata, Backend->Impl.MetadataSize);
  close(Backend->Impl.PayloadFd);
  close(Backend->Impl.MetadataFd);
  pthread_mutex_destroy(&Backend->Impl.BridgeInitMutex);
  Backend->~md_backend();
  munmap(Backend, sizeof(md_backend));
}

extern "C" int md_acquire(md_backend *Backend, size_t Size, size_t Alignment,
                            uint32_t Owner, md_lease *Out) {
  return md_acquire_flags(Backend, Size, Alignment, Owner, 0, Out);
}

// No free candidate fit. Descriptor creation and range initialization are
// isolated from the common shared-pool reuse path; caller still holds the lock.
static __attribute__((noinline)) int acquireBumpLocked(
    ArenaImpl &Impl, uint32_t NeededPages, uint32_t AlignmentPages,
    uint32_t Owner, md_lease *Out) {
  SharedMetadata *Metadata = Impl.Metadata;
  uint32_t StartPage =
      (Metadata->BumpPage + AlignmentPages - 1) & ~(AlignmentPages - 1);
  uint32_t TotalPages = static_cast<uint32_t>(Impl.Capacity >> Impl.PageShift);
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
  Run.State.store(Claiming, md_atomic::memory_order_relaxed);
  Out->block_id = Index;
  Out->owner = Owner;
  Out->generation = Generation;
  Out->address = reinterpret_cast<uintptr_t>(Impl.Payload) +
                 static_cast<uintptr_t>(StartPage) * Impl.PageSize;
  Out->size = static_cast<size_t>(NeededPages) * Impl.PageSize;
  MD_STAT(Metadata->AcquireSuccesses.incrementLocked());
  return 0;
}

static __attribute__((noinline)) int acquireLocal(md_arena_view *Backend, size_t Size, size_t Alignment,
                                  uint32_t Owner, unsigned Flags, md_lease *Out) {
  // The public entry validated arguments and acquired the owner Arena Mutex.
  ArenaImpl &Impl = Backend->Impl;
  SharedMetadata *Metadata = Impl.Metadata;
  uint32_t NeededPages = static_cast<uint32_t>(
      (Size + Impl.PageSize - 1) >> Impl.PageShift);
  uint32_t AlignmentPages = static_cast<uint32_t>(
      (Alignment + Impl.PageSize - 1) >> Impl.PageShift);
  if (AlignmentPages == 0)
    AlignmentPages = 1;
  // Page size and nonzero alignment are powers of two; no rounding loop.
  { MD_SCOPE(4, DrainProbe); drainLocked(Metadata, true); }
  MD_SCOPE(5, CandidateProbe);
  if (Metadata->KernelError) { int Error = Metadata->KernelError; unlock(Metadata); return Error; }
#ifdef MD_TEST_MIGRATION
  md_test_metadata_locked();
#endif
  MD_STAT(Metadata->AcquireCalls.incrementLocked());
  uint32_t StartClass = sizeClass(NeededPages);
  for (uint32_t Class = StartClass; Class < kNumBins; ++Class) {
    for (uint32_t Index = Metadata->BinHeads[Class]; Index != kNoIndex;
         Index = Metadata->Runs[Index].BinNext) {
      RunDescriptor &Run = Metadata->Runs[Index];
      if (Run.State.load(md_atomic::memory_order_relaxed) != Free)
        continue;
      int Result;
      if (Run.Pages == NeededPages &&
          (Run.StartPage & (AlignmentPages - 1)) == 0) {
        claimWholeRun(&Impl, Index, Owner, Out);
        Result = 0;
      } else {
        uint32_t StartPage;
        if (!alignedStart(Run.StartPage, Run.Pages, NeededPages, AlignmentPages,
                          &StartPage))
          continue;
        Result = claimRun(&Impl, Index, StartPage, NeededPages, Owner, Out);
      }
      if (Result == 0) {
        MD_STAT(Metadata->AcquireSuccesses.incrementLocked());
        MD_STOP(CandidateProbe);
        return finishAcquireLocked(Impl, *Out, Flags);
      }
      unlock(Metadata);
      return ENOSPC;
    }
  }
  int Result = acquireBumpLocked(Impl, NeededPages, AlignmentPages, Owner, Out);
  MD_STOP(CandidateProbe);
  return Result ? Result : finishAcquireLocked(Impl, *Out, Flags);
}

static int tryAcquireLocal(md_arena_view *Backend, md_candidate Candidate,
                                uint32_t Owner, md_lease *Out) {
  if (Backend == nullptr || Out == nullptr || Candidate.block_id >= kMaxRuns)
    return EINVAL;
  SharedMetadata *Metadata = Backend->Impl.Metadata;
  lock(Metadata);
  if ((Backend->Impl.ArenaCount > 1 || Backend->Impl.KernelBridge) &&
      currentCpuFast() != static_cast<int>(Metadata->ArenaId)) {
    unlock(Metadata);
    return EAGAIN;
  }
  drainLocked(Metadata);
  if (Metadata->KernelError) { int Error = Metadata->KernelError; unlock(Metadata); return Error; }
#ifdef MD_TEST_MIGRATION
  md_test_metadata_locked();
#endif
  MD_STAT(Metadata->AcquireCalls.incrementLocked());
  RunDescriptor &Run = Metadata->Runs[Candidate.block_id];
  if (Run.State.load(md_atomic::memory_order_relaxed) != Free ||
      Run.Generation.load(md_atomic::memory_order_acquire) != Candidate.generation) {
    unlock(Metadata);
    return EAGAIN;
  }
  unlinkBin(Metadata, Candidate.block_id);
  unlinkAddress(Metadata, Candidate.block_id);
  Run.Owner = Owner;
  uint64_t Generation =
      Run.Generation.incrementLocked() + 1;
  Run.State.store(Claiming, md_atomic::memory_order_relaxed);
  Out->block_id = Candidate.block_id;
  Out->owner = Owner;
  Out->generation = Generation;
  Out->address = reinterpret_cast<uintptr_t>(Backend->Impl.Payload) +
                 static_cast<uintptr_t>(Run.StartPage) * Backend->Impl.PageSize;
  Out->size = static_cast<size_t>(Run.Pages) * Backend->Impl.PageSize;
  MD_STAT(Metadata->AcquireSuccesses.incrementLocked());
  return finishAcquireLocked(Backend->Impl, *Out, 0);
}

static int releaseLocal(md_arena_view *Backend, const md_lease *Lease,
                         int ClearPayload, md_candidate *OutCandidate) {
  if (!Backend || !Lease || Lease->block_id >= kMaxRuns) return EINVAL;
  ArenaImpl &Impl = Backend->Impl;
  SharedMetadata *Metadata = Impl.Metadata;
  lock(Metadata);
  drainLocked(Metadata);
  RunDescriptor &Run = Metadata->Runs[Lease->block_id];
  if (Metadata->KernelError) { int Error = Metadata->KernelError; unlock(Metadata); return Error; }
  if (Lease->owner != md_process_id() || Run.State.load(__ATOMIC_RELAXED) != InUse ||
      Run.ReleaseTicket.load(__ATOMIC_RELAXED) != Lease->generation ||
      Run.Owner != Lease->owner || Run.Generation.load(__ATOMIC_RELAXED) != Lease->generation ||
      Run.AllocationId != Lease->allocation_id || Run.KernelSession != Lease->kernel_session ||
      Lease->kernel_session != md_kernel_bridge_session(Impl.KernelBridge) ||
      Lease->address != reinterpret_cast<uintptr_t>(Impl.Payload) + size_t(Run.StartPage) * Impl.PageSize ||
      Lease->size != size_t(Run.Pages) * Impl.PageSize) {
    unlock(Metadata); return ESTALE;
  }
#ifdef MD_TEST_MIGRATION
  md_test_metadata_locked();
#endif
  if (ClearPayload) memset(reinterpret_cast<void *>(Lease->address), 0, Lease->size);
  if (Impl.KernelBridge) {
    md_shared_record Record{};
    Record.session = Lease->kernel_session;
    Record.block_id = uint64_t(Metadata->ArenaId) * kMaxRuns + Lease->block_id + 1;
    Record.generation = Lease->generation;
    Record.allocation_id = Lease->allocation_id;
    Record.start_page = Run.StartPage;
    Record.nr_pages = Run.Pages;
    Record.cpu = static_cast<uint16_t>(Metadata->ArenaId);
    Record.op = MD_LOG_FREE;
    Run.LastOwner = ClearPayload ? kNoOwner : Lease->owner;
    Run.LastSession = ClearPayload ? 0 : Run.HolderSession;
    Run.State.store(Pending, __ATOMIC_RELAXED);
    Run.ReleaseTicket.store(0, __ATOMIC_RELAXED);
    uint64_t Ticket;
    int Error = md_kernel_bridge_normal_ticket(Impl.KernelBridge, &Record, &Ticket);
    if (Error) {
      Run.State.store(InUse, __ATOMIC_RELAXED);
      Run.ReleaseTicket.store(Lease->generation, __ATOMIC_RELAXED);
      unlock(Metadata); return Error;
    }
    MD_STAT(Metadata->ReleaseCalls.incrementLocked());
    // Check AFTER publishing the log. If we migrate after this check, the
    // owner CPU's schedule-out observes this FREE before another mm can run.
    // A holder that has already migrated must keep Pending until its receipt.
    if (currentCpuFast() == static_cast<int>(Metadata->ArenaId)) {
      uint32_t ResultIndex = publishLocked(Metadata, Lease->block_id);
      MD_STAT(Metadata->ReleaseSuccesses.incrementLocked());
      if (OutCandidate) *OutCandidate = {ResultIndex,
          Metadata->Runs[ResultIndex].Generation.load(__ATOMIC_RELAXED)};
    } else {
      Run.ReleaseTicket.store(Ticket, __ATOMIC_RELAXED);
      Run.Next = Metadata->PendingHead;
      Metadata->PendingHead = Lease->block_id;
      if (OutCandidate) *OutCandidate = {kNoIndex, 0};
    }
    unlock(Metadata);
    return 0;
  }
  Run.LastOwner = ClearPayload ? kNoOwner : Lease->owner;
  Run.LastSession = ClearPayload ? 0 : Run.HolderSession;
  Run.ReleaseTicket.store(0, __ATOMIC_RELAXED);
  uint32_t ResultIndex = publishLocked(Metadata, Lease->block_id);
  MD_STAT(Metadata->ReleaseCalls.incrementLocked());
  MD_STAT(Metadata->ReleaseSuccesses.incrementLocked());
  if (OutCandidate) *OutCandidate = {ResultIndex,
      Metadata->Runs[ResultIndex].Generation.load(__ATOMIC_RELAXED)};
  unlock(Metadata);
  return 0;
}

static int leaseForAddressLocal(md_arena_view *Backend, uintptr_t Address,
                                      uint32_t Owner, md_lease *Out) {
  if (Backend == nullptr || Out == nullptr) return EINVAL;
  ArenaImpl &Impl = Backend->Impl;
  uintptr_t Base = reinterpret_cast<uintptr_t>(Impl.Payload);
  if (Address < Base || Address - Base >= Impl.Capacity) return EINVAL;
  uint32_t StartPage = static_cast<uint32_t>(
      (Address - reinterpret_cast<uintptr_t>(Impl.Payload)) / Impl.PageSize);
  SharedMetadata *Metadata = Impl.Metadata;
  lock(Metadata);
  for (uint32_t Index = 0; Index < Metadata->NextUnused; ++Index) {
    RunDescriptor &Run = Metadata->Runs[Index];
    if (Run.State.load(md_atomic::memory_order_relaxed) == InUse &&
        Run.StartPage == StartPage && Run.Owner == Owner) {
      Out->block_id = Index;
      Out->owner = Owner;
      Out->generation = Run.Generation.load(md_atomic::memory_order_acquire);
      Out->address = Address;
      Out->size = static_cast<size_t>(Run.Pages) * Impl.PageSize;
      Out->allocation_id = Run.AllocationId;
      Out->kernel_session = Run.KernelSession;
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
  if (!Backend || !md_contains(Backend, Address)) return EINVAL;
  uint32_t Arena = static_cast<uint32_t>((Address -
      reinterpret_cast<uintptr_t>(Backend->Impl.Payload)) / Backend->Impl.ArenaStride);
  // Remote release must carry its saved lease, never reconstruct it from metadata.
  if (md_current_arena(Backend) != static_cast<int>(Arena)) return EXDEV;
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
  if (Address < Base || Address >= Base + Backend->Impl.WindowSize)
    return 0;
  uintptr_t Offset = Address - Base;
  return Offset % Backend->Impl.ArenaStride < Backend->Impl.Capacity;
}

static void statsLocal(md_arena_view *Backend, md_stats *Out) {
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
  Out->remote_enqueued = __atomic_load_n(&sharedLog(Metadata)->remote.reserved, __ATOMIC_RELAXED);
  Out->kernel_rejected = Metadata->KernelRejected;
  Out->kernel_enabled = Metadata->KernelEnabled == 1;
  for (uint32_t Index = 0; Index < Metadata->NextUnused; ++Index) {
    RunDescriptor &Run = Metadata->Runs[Index];
    uint64_t Bytes = static_cast<uint64_t>(Run.Pages) * Backend->Impl.PageSize;
    uint32_t StateValue = Run.State.load(md_atomic::memory_order_relaxed);
    if (StateValue == InUse || StateValue == Claiming || StateValue == Pending || StateValue == Quarantined)
      Out->bytes_in_use += Bytes;
    else if (StateValue == Free)
      Out->bytes_free += Bytes;
  }
  unlock(Metadata);
}

extern "C" int md_cpu_current() { return currentCpuFast(); }
extern "C" const char *md_cpu_query_mode() {
  if (!CpuState.Mode) (void)initializeCpuReader();
  return CpuState.Mode == 1 ? "rseq_owned" : CpuState.Mode == 3 ? "rseq_libc" : "syscall_fallback";
}

extern "C" int md_current_arena(md_backend *Backend) {
  if (!Backend) return -1;
  if (Backend->Impl.ArenaCount == 1) return 0;
  int Cpu = currentCpuFast();
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
                   static_cast<uintptr_t>(Arena) * Backend->Impl.ArenaStride;
  Out->data_size = Backend->Impl.Capacity;
  Out->nr_pages = static_cast<uint32_t>(Backend->Impl.Capacity /
                                        Backend->Impl.PageSize);
  return 0;
}

extern "C" int md_acquire_flags(md_backend *Backend, size_t Size, size_t Alignment,
                                 uint32_t Owner, unsigned Flags, md_lease *Out) {
  MD_SCOPE(1, BackendProbe);
  MD_SCOPE(2, RouteProbe);
  if (!Backend || !Out || !Size ||
      (Alignment && (Alignment & (Alignment - 1))) || Owner == kNoOwner ||
      (Flags & ~static_cast<unsigned>(MD_ZERO))) return EINVAL;
  // Valid requests that cannot fit the configured pool are resource exhaustion,
  // not malformed arguments. The allocator adapter may use its native backend.
  if (Size > Backend->Impl.Capacity || Alignment > Backend->Impl.Capacity)
    return ENOMEM;
  int BridgeError = refreshKernelBridgesAfterFork(Backend);
  if (BridgeError) return BridgeError;
  for (;;) {
    int Arena = md_current_arena(Backend);
    if (Arena < 0) return ENOTSUP;
    md_arena_view View = arenaView(Backend, static_cast<uint32_t>(Arena));
    SharedMetadata *Metadata = View.Impl.Metadata;
    MD_STOP(RouteProbe);
    { MD_SCOPE(3, LockProbe); lock(Metadata); }
    int LockedCpu = currentCpuFast();
    if ((Backend->Impl.ArenaCount > 1 || View.Impl.KernelBridge) && LockedCpu != Arena) {
      unlock(Metadata);
      if (Backend->Impl.ArenaCount == 1) return EAGAIN;
      continue;
    }
    int Result = acquireLocal(&View, Size, Alignment, Owner, Flags, Out);
    if (!Result) Out->block_id += static_cast<uint32_t>(Arena) * kMaxRuns;
    return Result;
  }
}

extern "C" int md_try_acquire(md_backend *Backend, md_candidate Candidate,
                               uint32_t Owner, md_lease *Out) {
  if (!Backend || !Out || Owner == kNoOwner ||
      Candidate.block_id >= Backend->Impl.ArenaCount * kMaxRuns) return EINVAL;
  int BridgeError = refreshKernelBridgesAfterFork(Backend);
  if (BridgeError) return BridgeError;
  uint32_t Arena = Candidate.block_id / kMaxRuns;
  if (md_current_arena(Backend) != static_cast<int>(Arena)) return EAGAIN;
  md_arena_view View = arenaView(Backend, Arena);
  Candidate.block_id %= kMaxRuns;
  int Result = tryAcquireLocal(&View, Candidate, Owner, Out);
  if (!Result) Out->block_id += Arena * kMaxRuns;
  return Result;
}

extern "C" int md_release(md_backend *Backend, const md_lease *Lease,
                           int ClearPayload, md_candidate *Candidate) {
  if (!Backend || !Lease || Lease->block_id >= Backend->Impl.ArenaCount * kMaxRuns)
    return EINVAL;
  int BridgeError = refreshKernelBridgesAfterFork(Backend);
  if (BridgeError) return BridgeError;
  uint32_t Arena = Lease->block_id / kMaxRuns;
  md_arena_view View = arenaView(Backend, Arena);
  md_lease Local = *Lease;
  Local.block_id %= kMaxRuns;
  if (md_current_arena(Backend) == static_cast<int>(Arena)) {
    int Result = releaseLocal(&View, &Local, ClearPayload, Candidate);
    if (!Result && Candidate && Candidate->block_id != kNoIndex)
      Candidate->block_id += Arena * kMaxRuns;
    return Result;
  }
  // A remote release is built entirely from its lease. No descriptor read,
  // ReleaseTicket CAS, state write, generation change or Arena Mutex here.
  if (Local.owner != md_process_id() || !Local.allocation_id || !Local.generation ||
      Local.address < reinterpret_cast<uintptr_t>(View.Impl.Payload) ||
      Local.size == 0 || Local.size % View.Impl.PageSize ||
      Local.address % View.Impl.PageSize ||
      Local.address - reinterpret_cast<uintptr_t>(View.Impl.Payload) > View.Impl.Capacity ||
      Local.size > View.Impl.Capacity -
          (Local.address - reinterpret_cast<uintptr_t>(View.Impl.Payload)) ||
      Local.kernel_session != md_kernel_bridge_session(View.Impl.KernelBridge))
    return ESTALE;
  md_shared_record Record{};
  Record.session = Local.kernel_session;
  Record.block_id = uint64_t(Lease->block_id) + 1;
  Record.generation = Local.generation;
  Record.allocation_id = Local.allocation_id;
  Record.start_page = static_cast<uint32_t>((Local.address -
      reinterpret_cast<uintptr_t>(View.Impl.Payload)) / View.Impl.PageSize);
  Record.nr_pages = static_cast<uint32_t>(Local.size / View.Impl.PageSize);
  Record.cpu = static_cast<uint16_t>(Arena);
  Record.op = MD_LOG_FREE;
  int Result = md_kernel_log_remote(sharedLog(View.Impl.Metadata), &Record,
      ClearPayload ? MD_USER_FREE_CLEAR : 0);
  if (!Result && Candidate) *Candidate = {kNoIndex, 0};
  return Result;
}

extern "C" int md_drain_remote(md_backend *Backend, unsigned Arena) {
  if (!Backend || Arena >= Backend->Impl.ArenaCount) return EINVAL;
  if (md_current_arena(Backend) != static_cast<int>(Arena)) return EXDEV;
  SharedMetadata *Metadata = Backend->Impl.Metadata + Arena;
  lock(Metadata);
  drainLocked(Metadata);
  int Result = Metadata->KernelError;
  unlock(Metadata);
  return Result;
}

extern "C" int md_lease_for_address(md_backend *Backend, uintptr_t Address,
                                     uint32_t Owner, md_lease *Out) {
  if (!Backend || !Out || !md_contains(Backend, Address)) return EINVAL;
  uint32_t Arena = static_cast<uint32_t>(
      (Address - reinterpret_cast<uintptr_t>(Backend->Impl.Payload)) /
      Backend->Impl.ArenaStride);
  md_arena_view View = arenaView(Backend, Arena);
  int Result = leaseForAddressLocal(&View, Address, Owner, Out);
  if (!Result) Out->block_id += Arena * kMaxRuns;
  return Result;
}

extern "C" void md_get_stats(md_backend *Backend, md_stats *Out) {
  if (!Backend || !Out) return;
  memset(Out, 0, sizeof(*Out));
  for (uint32_t Arena = 0; Arena < Backend->Impl.ArenaCount; ++Arena) {
    md_arena_view View = arenaView(Backend, Arena);
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
    SUM(kernel_rejected); SUM(kernel_enabled);
#undef SUM
  }
}

extern "C" int md_backend_broker_create(md_backend **Out, const md_config *Config) {
  BrokerCreating = true;
  int Result = md_backend_open(Out, Config);
  BrokerCreating = false;
  return Result;
}
extern "C" int md_backend_broker_export(md_backend *Backend, int Fds[2]) {
  if (!Backend || !Fds) return EINVAL;
  Fds[0] = fcntl(Backend->Impl.MetadataFd, F_DUPFD_CLOEXEC, 0);
  if (Fds[0] < 0) return errno;
  Fds[1] = fcntl(Backend->Impl.PayloadFd, F_DUPFD_CLOEXEC, 0);
  if (Fds[1] < 0) { int Error = errno; close(Fds[0]); Fds[0] = -1; return Error; }
  return 0;
}
