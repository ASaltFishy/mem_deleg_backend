#include "include/backend_broker.h"
#include "include/uapi/linux/memory_delegation.h"
#include "include/backend_broker_protocol.h"
#include "include/memory_delegation_prctl.h"
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <time.h>
#include <stdint.h>

// Versioned, read-only query: no registration, mapping, or backing mutation.
static int checkKernelAbi() {
  errno = 0;
  int Result = prctl(PR_SET_MEMORY_DELEGATION_LOG, 0UL,
                     PR_MEMORY_DELEGATION_F_SHARED_RINGS, 0UL, MD_SHARED_LOG_QUERY);
  int Error = errno;
  if (Result == MD_SHARED_LOG_VERSION) {
    printf("KERNEL_ABI_OK shared_rings_v3\n");
    return 0;
  }
  fprintf(stderr, "KERNEL_ABI_UNSUPPORTED: shared_rings_v3 probe rc=%d errno=%d (%s).\n",
          Result, Error, strerror(Error));
  fprintf(stderr, "Boot the matching v3 kernel before running the delegated benchmark.\n");
  if (Error == EPERM || Error == EACCES)
    fprintf(stderr, "Permission denied: check root/SELinux before concluding the ABI is absent.\n");
  return 1;
}

// One maintenance owner per domain. No worker in application allocators.
struct Reclaimer {
  md_backend *Backend;
  md_reclaim_policy Policy;
};
static bool readOption(const char *Name, uint64_t Default, uint64_t *Out) {
  const char *Value = getenv(Name);
  if (!Value) { *Out = Default; return true; }
  if (!*Value || *Value == '-') return false;
  char *End; errno = 0;
  unsigned long long Parsed = strtoull(Value, &End, 10);
  if (errno || *End) return false;
  *Out = Parsed; return true;
}
static uint64_t monotonicNs() {
  timespec T; clock_gettime(CLOCK_MONOTONIC, &T);
  return uint64_t(T.tv_sec) * 1000000000ULL + T.tv_nsec;
}
static unsigned memoryPressure(uint64_t *PreviousStall, uint64_t *PreviousTime) {
  unsigned Level = 0;
  FILE *File = fopen("/proc/meminfo", "r");
  char Line[256]; unsigned long long Total = 0, Available = 0;
  bool HaveAvailable = false;
  if (File) {
    while (fgets(Line, sizeof(Line), File)) {
      if (sscanf(Line, "MemTotal: %llu kB", &Total) == 1) continue;
      if (sscanf(Line, "MemAvailable: %llu kB", &Available) == 1) HaveAvailable = true;
    }
    fclose(File);
    if (Total && HaveAvailable) {
      if (Available < Total / 20) Level = 2;
      else if (Available < Total / 10) Level = 1;
    }
  }
  File = fopen("/proc/pressure/memory", "r");
  unsigned long long Stall = 0; bool HaveStall = false;
  if (File) {
    while (fgets(Line, sizeof(Line), File)) {
      char *Value = strstr(Line, "total=");
      if (!strncmp(Line, "some ", 5) && Value && sscanf(Value, "total=%llu", &Stall) == 1)
        HaveStall = true;
    }
    fclose(File);
  }
  uint64_t Now = monotonicNs();
  if (HaveStall && *PreviousTime && Stall >= *PreviousStall && Now > *PreviousTime) {
    double Fraction = double(Stall - *PreviousStall) * 1000.0 / double(Now - *PreviousTime);
    if (Fraction >= 0.30) Level = 2;
    else if (Fraction >= 0.10 && Level < 1) Level = 1;
  }
  if (HaveStall) { *PreviousTime = Now; *PreviousStall = Stall; }
  return Level;
}
static void *reclaimWorker(void *Argument) {
  auto *Worker = static_cast<Reclaimer *>(Argument);
  uint64_t PreviousStall = 0, PreviousTime = 0, LastPressure = 0, LastReport = 0;
  unsigned Level = 0;
  bool Trimming = false, ContinueDrain = false;
  for (;;) {
    if (!ContinueDrain) md_backend_wait_reclaim(Worker->Backend, 200);
    uint64_t Now = monotonicNs();
    unsigned Observed = memoryPressure(&PreviousStall, &PreviousTime);
    if (Observed >= Level) { Level = Observed; if (Observed) LastPressure = Now; }
    else if (Now - LastPressure >= 5000000000ULL) { Level = Observed; LastPressure = Now; }
    md_reclaim_policy Policy = Worker->Policy;
    if (Level == 1) {
      Policy.high_bytes /= 4; Policy.low_bytes /= 4;
      if (!Policy.idle_ns || Policy.idle_ns > 200000000ULL) Policy.idle_ns = 200000000ULL;
    } else if (Level == 2) { Policy.high_bytes = Policy.low_bytes = 0; }
    if (Trimming) Policy.high_bytes = Policy.low_bytes;
    md_reclaim_result Result{};
    int Error = md_backend_reclaim(Worker->Backend, &Policy, &Result);
    uint64_t Remaining = Result.bytes_hot > Result.bytes_reclaimed ?
                         Result.bytes_hot - Result.bytes_reclaimed : 0;
    Trimming = (Trimming || Result.bytes_hot > Policy.high_bytes) && Remaining > Policy.low_bytes;
    if ((Error || Result.failures || Result.bytes_reclaimed) && Now - LastReport >= 1000000000ULL) {
      fprintf(stderr, "RECLAIM pressure=%u hot_estimate=%llu cold=%llu released=%llu failures=%llu skipped=%llu error=%d\n",
              Level, (unsigned long long)Result.bytes_hot, (unsigned long long)Result.bytes_cold,
              (unsigned long long)Result.bytes_reclaimed, (unsigned long long)Result.failures,
              (unsigned long long)Result.skipped, Error ? Error : Result.last_error);
      LastReport = Now;
    }
    // A nonempty backlog must not be throttled to one batch per timer tick.
    // Keep the configured per-call bounds, yield briefly between successful
    // batches, and back off on contention/errors instead of busy spinning.
    ContinueDrain = !Error && Result.bytes_reclaimed &&
                    (Trimming || Result.bytes_reclaimed >= Policy.batch_bytes);
    usleep(ContinueDrain ? 1000 : 50000);
  }
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--check-kernel")) return checkKernelAbi();
  if (argc != 3) { fprintf(stderr,"usage: %s SOCKET DOMAIN\n",argv[0]); return 2; }
  const char *Mode = getenv("MD_KERNEL_BRIDGE");
  if (Mode && !strcmp(Mode, "required") && checkKernelAbi()) return 1;
  Reclaimer Worker{};
  uint64_t IdleMs, BatchRuns;
  if (!readOption("MD_RECLAIM_HIGH_BYTES", 128ULL << 20, &Worker.Policy.high_bytes) ||
      !readOption("MD_RECLAIM_LOW_BYTES", 64ULL << 20, &Worker.Policy.low_bytes) ||
      !readOption("MD_RECLAIM_IDLE_MS", 2000, &IdleMs) ||
      !readOption("MD_RECLAIM_BATCH_BYTES", 16ULL << 20, &Worker.Policy.batch_bytes) ||
      !readOption("MD_RECLAIM_MAX_RUNS", 8, &BatchRuns) ||
      Worker.Policy.low_bytes > Worker.Policy.high_bytes ||
      IdleMs > UINT64_MAX / 1000000 || !BatchRuns || BatchRuns > 1024 ||
      Worker.Policy.batch_bytes < (uint64_t)sysconf(_SC_PAGESIZE)) {
    fprintf(stderr, "Invalid MD_RECLAIM_* policy\n"); return 2;
  }
  Worker.Policy.idle_ns = IdleMs * 1000000;
  Worker.Policy.max_runs = static_cast<uint32_t>(BatchRuns);
  sockaddr_un Address{}; Address.sun_family=AF_UNIX;
  if (strlen(argv[1]) >= sizeof(Address.sun_path)) return 2;
  strcpy(Address.sun_path,argv[1]);
  // Existing socket is an error. Never unlink a potentially live service.
  int Server=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
  if (Server<0 || bind(Server,(sockaddr*)&Address,sizeof(Address)) || listen(Server,16)) { perror("broker bind/listen"); return 1; }
  chmod(argv[1],0600);
  md_config Config{argv[2],0x2000000000ULL,MD_DEFAULT_ARENA_CAPACITY,0};
  md_backend *Backend=nullptr;
  int Error=md_backend_broker_create(&Backend,&Config);
  if(Error) { fprintf(stderr,"broker create error=%d (%s)\n",Error,strerror(Error)); unlink(argv[1]); return 1; }
  Worker.Backend = Backend;
  const char *ReclaimMode = getenv("MD_RECLAIM");
  if (!ReclaimMode || strcmp(ReclaimMode, "0")) {
    md_backend_reclaim_notifications(Backend, 1);
    pthread_t Thread;
    Error = pthread_create(&Thread, nullptr, reclaimWorker, &Worker);
    if (Error) { fprintf(stderr, "reclaimer startup: %s\n", strerror(Error)); return 1; }
    pthread_detach(Thread);
  }
  printf("BACKEND_BROKER_READY version=%u domain=%s\n",MD_BROKER_VERSION,argv[2]); fflush(stdout);
  for(;;) {
    int Client=accept4(Server,nullptr,nullptr,SOCK_CLOEXEC); if(Client<0) { if(errno==EINTR) continue; return 1; }
    md_broker_request Request{};
    md_broker_response Response{MD_BROKER_MAGIC,MD_BROKER_VERSION,0,0};
    ssize_t Count=recv(Client,&Request,sizeof(Request),MSG_TRUNC);
    if(Count!=sizeof(Request) || Request.magic!=MD_BROKER_MAGIC || Request.version!=MD_BROKER_VERSION ||
       Request.page_size!=(uint32_t)sysconf(_SC_PAGESIZE) || Request.arena_count!=md_arena_count(Backend) ||
       Request.capacity!=Config.capacity || Request.fixed_address!=Config.fixed_address ||
       !memchr(Request.domain,0,sizeof(Request.domain)) || strcmp(Request.domain,Config.domain)) Response.error=EPROTO;
    int Fds[2]{-1,-1};
    if(!Response.error) Response.error=md_backend_broker_export(Backend,Fds);
    iovec Iov{&Response,sizeof(Response)};
    alignas(cmsghdr) unsigned char Control[CMSG_SPACE(sizeof(Fds))]{};
    msghdr Message{}; Message.msg_iov=&Iov; Message.msg_iovlen=1;
    if(!Response.error) {
      Message.msg_control=Control; Message.msg_controllen=sizeof(Control);
      cmsghdr *Header=CMSG_FIRSTHDR(&Message); Header->cmsg_level=SOL_SOCKET; Header->cmsg_type=SCM_RIGHTS;
      Header->cmsg_len=CMSG_LEN(sizeof(Fds)); memcpy(CMSG_DATA(Header),Fds,sizeof(Fds));
    }
    (void)sendmsg(Client,&Message,MSG_NOSIGNAL);
    if(Fds[0]>=0) close(Fds[0]);
    if(Fds[1]>=0) close(Fds[1]);
    close(Client);
  }
}
