#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/net.h"
#include "user/user.h"

#define NC_BUFSZ 512

static void
usage(void)
{
  fprintf(2, "Usage: netcat -l <port>\n");
  fprintf(2, "       netcat <ip> <port>\n");
  exit(1);
}

// Parse dotted IPv4 text (for example "10.0.2.2") into host byte order.
static int
parse_ipv4(const char *s, uint32 *out)
{
  int octets[4] = {0, 0, 0, 0};
  int idx = 0;
  int val = 0;
  int have_digit = 0;

  for(; *s; s++){
    if(*s >= '0' && *s <= '9'){
      val = val * 10 + (*s - '0');
      if(val > 255)
        return -1;
      have_digit = 1;
    } else if(*s == '.'){
      if(!have_digit || idx >= 3)
        return -1;
      octets[idx++] = val;
      val = 0;
      have_digit = 0;
    } else {
      return -1;
    }
  }

  if(idx != 3 || !have_digit)
    return -1;

  octets[3] = val;
  *out = MAKE_IP_ADDR(octets[0], octets[1], octets[2], octets[3]);
  return 0;
}

static int
auto_connect(uint32 dst, uint16 dport)
{
  int pid = getpid();
  int base = 40000 + (pid % 1024);

  // Try a small range of source ports, similar to ephemeral ports.
  for(int i = 0; i < 128; i++){
    uint16 sport = (uint16)(base + i);
    int conn = tcpconnect(sport, dst, dport);
    if(conn >= 0)
      return conn;
  }
  return -1;
}

static void
relay(int conn)
{
  int cpid = fork();
  if(cpid < 0){
    fprintf(2, "netcat: fork failed\n");
    tcpclose(conn);
    exit(1);
  }

  if(cpid == 0){
    // Child: stdin -> TCP.
    char buf[NC_BUFSZ];
    for(;;){
      int n = read(0, buf, sizeof(buf));
      if(n <= 0)
        break;

      int off = 0;
      while(off < n){
        int m = tcpsend(conn, buf + off, n - off);
        if(m <= 0)
          exit(0);
        off += m;
      }
    }
    // Keep parent as the owner of tcpclose().
    exit(0);
  }

  // Parent: TCP -> stdout.
  char buf[NC_BUFSZ];
  for(;;){
    int n = tcprecv(conn, buf, sizeof(buf));
    if(n < 0){
      fprintf(2, "netcat: tcprecv failed\n");
      break;
    }
    if(n == 0)
      break;

    int off = 0;
    while(off < n){
      int m = write(1, buf + off, n - off);
      if(m <= 0)
        break;
      off += m;
    }
  }

  kill(cpid);
  wait(0);
  tcpclose(conn);
}

int
main(int argc, char **argv)
{
  if(argc == 3 && strcmp(argv[1], "-l") == 0){
    int port = atoi(argv[2]);
    if(port <= 0 || port > 65535)
      usage();

    int lid = tcplisten((uint16)port);
    if(lid < 0){
      fprintf(2, "netcat: tcplisten failed\n");
      exit(1);
    }

    uint32 src = 0;
    uint16 sport = 0;
    int conn = tcpaccept(lid, &src, &sport);
    if(conn < 0){
      fprintf(2, "netcat: tcpaccept failed\n");
      tcpclose(lid);
      exit(1);
    }

    // Only keep one connection, like a minimal `nc -l`.
    tcpclose(lid);
    relay(conn);
    exit(0);
  }

  if(argc == 3){
    uint32 dst;
    int port = atoi(argv[2]);
    if(port <= 0 || port > 65535)
      usage();
    if(parse_ipv4(argv[1], &dst) < 0)
      usage();

    int conn = auto_connect(dst, (uint16)port);
    if(conn < 0){
      fprintf(2, "netcat: tcpconnect failed\n");
      exit(1);
    }

    relay(conn);
    exit(0);
  }

  usage();
  return 0;
}
