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

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--check-kernel")) return checkKernelAbi();
  if (argc != 3) { fprintf(stderr,"usage: %s SOCKET DOMAIN\n",argv[0]); return 2; }
  const char *Mode = getenv("MD_KERNEL_BRIDGE");
  if (Mode && !strcmp(Mode, "required") && checkKernelAbi()) return 1;
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
