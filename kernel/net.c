#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses.
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

//
// ------------------------- UDP data path -------------------------
//

#define MAX_QUEUED_PACKETS 16
struct port_queue_mapping_entry {
  uint16 port; // 0 means unused entry.
  struct spinlock lock;
  uint16 head, tail, size;
  uint64 queued_packets[MAX_QUEUED_PACKETS];
};

#define MAX_MAPPINGS 64
static struct port_queue_mapping_entry port_queue_mappings[MAX_MAPPINGS];

static int
push_back(struct port_queue_mapping_entry *entry, void *addr)
{
  if(!holding(&entry->lock))
    panic("push_back: not holding");
  if(entry->size == MAX_QUEUED_PACKETS)
    return -1;

  entry->queued_packets[entry->tail] = (uint64)addr;
  entry->tail = (entry->tail + 1) % MAX_QUEUED_PACKETS;
  entry->size++;
  return 0;
}

static uint64
pop_front(struct port_queue_mapping_entry *entry)
{
  uint64 addr;

  if(!holding(&entry->lock))
    panic("pop_front: not holding");
  if(entry->size == 0)
    panic("pop_front: size=0");

  addr = entry->queued_packets[entry->head];
  entry->head = (entry->head + 1) % MAX_QUEUED_PACKETS;
  entry->size--;
  return addr;
}

//
// ------------------------- TCP data path -------------------------
//

// This TCP stack is intentionally minimal:
// - one global connection table
// - no TCP options
// - in-order receive only
// - stop-and-wait transmit with timeout retransmission

#define TCP_MAX_CONN        64
#define TCP_LISTEN_BACKLOG   8
#define TCP_RX_BUF        2048
#define TCP_TX_CHUNK       512
#define TCP_RETX_TICKS       5
#define TCP_MAX_RETRY       20

enum tcp_state {
  TCP_STATE_UNUSED = 0,
  TCP_STATE_LISTEN,
  TCP_STATE_SYN_SENT,
  TCP_STATE_SYN_RCVD,
  TCP_STATE_ESTABLISHED,
  TCP_STATE_CLOSE_WAIT,
  TCP_STATE_FIN_WAIT_1,
  TCP_STATE_FIN_WAIT_2,
  TCP_STATE_CLOSING,
  TCP_STATE_LAST_ACK,
  TCP_STATE_CLOSED,
};

struct tcp_conn {
  int used;
  int state;

  // Parent listener index for accepted child connections.
  int parent;

  // 4-tuple.
  uint16 lport;
  uint32 rip;
  uint16 rport;

  // Sequence state.
  uint32 iss;
  uint32 snd_una;
  uint32 snd_nxt;
  uint32 rcv_nxt;

  int reset;
  int peer_fin;

  // Listener backlog queue.
  int acc_head;
  int acc_tail;
  int acc_len;
  int acc_q[TCP_LISTEN_BACKLOG];

  // Receive stream buffer.
  int rx_head;
  int rx_len;
  char rx_buf[TCP_RX_BUF];
};

static struct spinlock tcplock;
static struct tcp_conn tcp_conns[TCP_MAX_CONN];
static uint32 tcp_iss = 1;

static struct tcp_conn *tcp_conn_from_id_locked(int id);
static void tcp_handle_segment_locked(struct tcp_conn *conn, struct tcp *tcp,
                                      char *payload, int payloadlen);

// 32-bit TCP sequence comparison helpers.
static int
seq_ge(uint32 a, uint32 b)
{
  return (int)(a - b) >= 0;
}

static int
seq_le(uint32 a, uint32 b)
{
  return (int)(a - b) <= 0;
}

static int
tcp_conn_id(struct tcp_conn *conn)
{
  return (int)(conn - tcp_conns) + 1;
}

static void
tcp_reset_conn(struct tcp_conn *conn)
{
  memset(conn, 0, sizeof(*conn));
  conn->parent = -1;
}

static struct tcp_conn *
tcp_alloc_conn_locked(void)
{
  for(int i = 0; i < TCP_MAX_CONN; i++){
    if(tcp_conns[i].used == 0){
      tcp_reset_conn(&tcp_conns[i]);
      tcp_conns[i].used = 1;
      return &tcp_conns[i];
    }
  }
  return 0;
}

static struct tcp_conn *
tcp_conn_from_id_locked(int id)
{
  if(id < 1 || id > TCP_MAX_CONN)
    return 0;
  struct tcp_conn *conn = &tcp_conns[id - 1];
  if(conn->used == 0)
    return 0;
  return conn;
}

static struct tcp_conn *
tcp_find_listener_locked(uint16 port)
{
  for(int i = 0; i < TCP_MAX_CONN; i++){
    if(tcp_conns[i].used && tcp_conns[i].state == TCP_STATE_LISTEN &&
       tcp_conns[i].lport == port)
      return &tcp_conns[i];
  }
  return 0;
}

static struct tcp_conn *
tcp_find_conn_locked(uint32 src_ip, uint16 src_port, uint16 dst_port)
{
  for(int i = 0; i < TCP_MAX_CONN; i++){
    struct tcp_conn *conn = &tcp_conns[i];
    if(conn->used == 0)
      continue;
    if(conn->state == TCP_STATE_LISTEN)
      continue;

    if(conn->lport == dst_port && conn->rip == src_ip && conn->rport == src_port)
      return conn;
  }
  return 0;
}

static int
tcp_lport_busy_locked(uint16 lport)
{
  for(int i = 0; i < TCP_MAX_CONN; i++){
    if(tcp_conns[i].used && tcp_conns[i].lport == lport)
      return 1;
  }
  return 0;
}

static int
tcp_enqueue_accept_locked(struct tcp_conn *listener, int child_id)
{
  if(listener->acc_len >= TCP_LISTEN_BACKLOG)
    return -1;

  listener->acc_q[listener->acc_tail] = child_id;
  listener->acc_tail = (listener->acc_tail + 1) % TCP_LISTEN_BACKLOG;
  listener->acc_len++;
  return 0;
}

static int
tcp_dequeue_accept_locked(struct tcp_conn *listener)
{
  int id;

  if(listener->acc_len == 0)
    return -1;

  id = listener->acc_q[listener->acc_head];
  listener->acc_head = (listener->acc_head + 1) % TCP_LISTEN_BACKLOG;
  listener->acc_len--;
  return id;
}

static int
tcp_rx_copy_locked(struct tcp_conn *conn, char *payload, int payloadlen)
{
  int room = TCP_RX_BUF - conn->rx_len;
  int copylen = payloadlen < room ? payloadlen : room;

  for(int i = 0; i < copylen; i++){
    int tail = (conn->rx_head + conn->rx_len) % TCP_RX_BUF;
    conn->rx_buf[tail] = payload[i];
    conn->rx_len++;
  }

  return copylen;
}

static uint16
tcp_adv_window_locked(struct tcp_conn *conn)
{
  int room = TCP_RX_BUF - conn->rx_len;
  if(room < 0)
    room = 0;
  if(room > 0xffff)
    room = 0xffff;
  return (uint16)room;
}

//
// Checksum helpers: compute Internet checksum over raw bytes.
//

static uint32
checksum_add_bytes(uint32 sum, const uint8 *data, int len)
{
  while(len > 1){
    sum += ((uint16)data[0] << 8) | data[1];
    data += 2;
    len -= 2;
  }
  if(len == 1)
    sum += ((uint16)data[0] << 8);
  return sum;
}

static uint16
checksum_finish(uint32 sum)
{
  while(sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return (uint16)(~sum);
}

static uint16
tcp_checksum(struct ip *ip, struct tcp *tcp, int tcp_len)
{
  uint32 sum = 0;
  uint16 nlen = htons(tcp_len);
  uint8 pseudo[12];

  // Pseudo header: src/dst/proto/segment length.
  memmove(pseudo + 0, &ip->ip_src, sizeof(ip->ip_src));
  memmove(pseudo + 4, &ip->ip_dst, sizeof(ip->ip_dst));
  pseudo[8] = 0;
  pseudo[9] = IPPROTO_TCP;
  memmove(pseudo + 10, &nlen, sizeof(nlen));

  sum = checksum_add_bytes(sum, pseudo, sizeof(pseudo));
  sum = checksum_add_bytes(sum, (uint8 *)tcp, tcp_len);
  return checksum_finish(sum);
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  while(nleft > 1){
    sum += *w++;
    nleft -= 2;
  }

  if(nleft == 1){
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);

  answer = ~sum;
  return answer;
}

// Build and transmit one TCP segment.
// Returns 0 on success and -1 if tx ring is full.
static int
tcp_send_segment(uint32 dst_ip, uint16 sport, uint16 dport,
                 uint32 seq, uint32 ack, uint8 flags, uint16 win,
                 char *payload, int payloadlen)
{
  int iplen = sizeof(struct ip) + sizeof(struct tcp) + payloadlen;
  int total = sizeof(struct eth) + iplen;

  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0)
    return -1;
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *)buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45;
  ip->ip_tos = 0;
  ip->ip_len = htons(iplen);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_TCP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst_ip);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct tcp *tcp = (struct tcp *)(ip + 1);
  tcp->sport = htons(sport);
  tcp->dport = htons(dport);
  tcp->seq = htonl(seq);
  tcp->ack = htonl(ack);
  tcp->off = (sizeof(struct tcp) / 4) << 4;
  tcp->flags = flags;
  tcp->win = htons(win);
  tcp->sum = 0;
  tcp->urp = 0;

  if(payloadlen > 0)
    memmove((char *)(tcp + 1), payload, payloadlen);

  // The checksum helper returns host-order value; TCP header field is network order.
  tcp->sum = htons(tcp_checksum(ip, tcp, sizeof(struct tcp) + payloadlen));

  if(e1000_transmit(buf, total) < 0){
    kfree(buf);
    return -1;
  }
  return 0;
}

static void
tcp_send_ack_locked(struct tcp_conn *conn)
{
  tcp_send_segment(conn->rip, conn->lport, conn->rport,
                   conn->snd_nxt, conn->rcv_nxt,
                   TCP_ACK, tcp_adv_window_locked(conn), 0, 0);
}

static void
tcp_send_rst_for_unmatched(struct ip *ip, struct tcp *tcp, int payloadlen)
{
  uint8 flags = TCP_RST;
  uint32 seq;
  uint32 ack = 0;
  uint16 sport = ntohs(tcp->dport);
  uint16 dport = ntohs(tcp->sport);
  uint32 dst = ntohl(ip->ip_src);

  if(tcp->flags & TCP_ACK){
    // RFC behavior: if ACK set, reply with bare RST using ACK as seq.
    seq = ntohl(tcp->ack);
  } else {
    // Otherwise acknowledge the incoming sequence space.
    int seglen = payloadlen;
    if(tcp->flags & TCP_SYN)
      seglen++;
    if(tcp->flags & TCP_FIN)
      seglen++;
    seq = 0;
    ack = ntohl(tcp->seq) + seglen;
    flags |= TCP_ACK;
  }

  tcp_send_segment(dst, sport, dport, seq, ack, flags, TCP_RX_BUF, 0, 0);
}

static void
tcp_close_with_rst_locked(struct tcp_conn *conn)
{
  if(conn->state != TCP_STATE_LISTEN){
    tcp_send_segment(conn->rip, conn->lport, conn->rport,
                     conn->snd_nxt, conn->rcv_nxt,
                     TCP_RST | TCP_ACK, tcp_adv_window_locked(conn), 0, 0);
  }
  tcp_reset_conn(conn);
}

static void
tcp_handle_ack_locked(struct tcp_conn *conn, uint32 ack)
{
  // Accept only ACKs in [snd_una, snd_nxt].
  if(seq_ge(ack, conn->snd_una) && seq_le(ack, conn->snd_nxt))
    conn->snd_una = ack;

  if(conn->state == TCP_STATE_FIN_WAIT_1 && seq_ge(conn->snd_una, conn->snd_nxt))
    conn->state = TCP_STATE_FIN_WAIT_2;
  else if(conn->state == TCP_STATE_CLOSING && seq_ge(conn->snd_una, conn->snd_nxt))
    conn->state = TCP_STATE_CLOSED;
  else if(conn->state == TCP_STATE_LAST_ACK && seq_ge(conn->snd_una, conn->snd_nxt))
    conn->state = TCP_STATE_CLOSED;
}

static void
tcp_handle_segment_locked(struct tcp_conn *conn, struct tcp *tcp,
                          char *payload, int payloadlen)
{
  uint8 flags = tcp->flags;
  uint32 seq = ntohl(tcp->seq);
  uint32 ack = ntohl(tcp->ack);

  if(flags & TCP_RST){
    conn->reset = 1;
    conn->state = TCP_STATE_CLOSED;
    wakeup(conn);
    return;
  }

  if(conn->state == TCP_STATE_SYN_SENT){
    if((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
       ack == conn->snd_nxt){
      conn->snd_una = ack;
      conn->rcv_nxt = seq + 1;
      conn->state = TCP_STATE_ESTABLISHED;
      tcp_send_ack_locked(conn);
      wakeup(conn);
    }
    return;
  }

  if(conn->state == TCP_STATE_SYN_RCVD){
    // Duplicate SYN: retransmit SYN-ACK.
    if((flags & TCP_SYN) && !(flags & TCP_ACK)){
      tcp_send_segment(conn->rip, conn->lport, conn->rport,
                       conn->iss, conn->rcv_nxt,
                       TCP_SYN | TCP_ACK, tcp_adv_window_locked(conn), 0, 0);
      return;
    }

    // Final ACK of the three-way handshake.
    if((flags & TCP_ACK) && ack == conn->snd_nxt){
      conn->snd_una = ack;
      conn->state = TCP_STATE_ESTABLISHED;

      if(conn->parent >= 0 && conn->parent < TCP_MAX_CONN){
        struct tcp_conn *listener = &tcp_conns[conn->parent];
        if(listener->used && listener->state == TCP_STATE_LISTEN){
          if(tcp_enqueue_accept_locked(listener, tcp_conn_id(conn)) == 0)
            wakeup(listener);
          else
            tcp_close_with_rst_locked(conn);
        } else {
          tcp_close_with_rst_locked(conn);
        }
      }
      wakeup(conn);
    }
    return;
  }

  if(flags & TCP_ACK)
    tcp_handle_ack_locked(conn, ack);

  // Only in-order receive is supported.
  if(payloadlen > 0){
    if(seq == conn->rcv_nxt){
      int copied = tcp_rx_copy_locked(conn, payload, payloadlen);
      conn->rcv_nxt += copied;
      if(copied > 0)
        wakeup(conn);
      tcp_send_ack_locked(conn);
    } else {
      // Out-of-order data: send duplicate ACK for current rcv_nxt.
      tcp_send_ack_locked(conn);
    }
  }

  if(flags & TCP_FIN){
    if(seq + payloadlen == conn->rcv_nxt){
      conn->rcv_nxt++;
      conn->peer_fin = 1;

      if(conn->state == TCP_STATE_ESTABLISHED)
        conn->state = TCP_STATE_CLOSE_WAIT;
      else if(conn->state == TCP_STATE_FIN_WAIT_1){
        if(seq_ge(conn->snd_una, conn->snd_nxt))
          conn->state = TCP_STATE_CLOSED;
        else
          conn->state = TCP_STATE_CLOSING;
      } else if(conn->state == TCP_STATE_FIN_WAIT_2){
        conn->state = TCP_STATE_CLOSED;
      }

      tcp_send_ack_locked(conn);
      wakeup(conn);
    } else {
      // FIN for data we haven't accepted yet.
      tcp_send_ack_locked(conn);
    }
  }

  if(conn->state == TCP_STATE_CLOSED)
    wakeup(conn);
}

void
netinit(void)
{
  initlock(&tcplock, "tcp");

  for(int i = 0; i < MAX_MAPPINGS; i++){
    initlock(&port_queue_mappings[i].lock, "udp_port");
    port_queue_mappings[i].port = 0;
    port_queue_mappings[i].head = 0;
    port_queue_mappings[i].tail = 0;
    port_queue_mappings[i].size = 0;
  }

  for(int i = 0; i < TCP_MAX_CONN; i++)
    tcp_reset_conn(&tcp_conns[i]);
}

//
// bind(int port)
// prepare to receive UDP packets addressed to port.
//
uint64
sys_bind(void)
{
  int port;
  argint(0, &port);

  if(port <= 0 || port > 0xffff)
    return -1;

  struct port_queue_mapping_entry *free_entry = 0;

  for(int i = 0; i < MAX_MAPPINGS; i++){
    acquire(&port_queue_mappings[i].lock);
    if(port_queue_mappings[i].port == port){
      release(&port_queue_mappings[i].lock);
      return -1;
    }
    if(port_queue_mappings[i].port == 0 && free_entry == 0)
      free_entry = &port_queue_mappings[i];
    release(&port_queue_mappings[i].lock);
  }

  if(free_entry == 0)
    return -1;

  acquire(&free_entry->lock);
  free_entry->port = port;
  free_entry->head = 0;
  free_entry->tail = 0;
  free_entry->size = 0;
  release(&free_entry->lock);

  return 0;
}

//
// unbind(int port)
// release resources previously created by bind(port).
//
uint64
sys_unbind(void)
{
  int port;
  argint(0, &port);

  for(int i = 0; i < MAX_MAPPINGS; i++){
    acquire(&port_queue_mappings[i].lock);
    if(port_queue_mappings[i].port == port){
      while(port_queue_mappings[i].size > 0){
        char *pkt = (char *)pop_front(&port_queue_mappings[i]);
        kfree(pkt);
      }
      port_queue_mappings[i].port = 0;
      release(&port_queue_mappings[i].lock);
      return 0;
    }
    release(&port_queue_mappings[i].lock);
  }

  return -1;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
//
uint64
sys_recv(void)
{
  struct proc *p = myproc();
  int dport;
  uint64 srcaddr_u;
  uint64 sport_u;
  uint64 buf_u;
  int maxlen;

  argint(0, &dport);
  argaddr(1, &srcaddr_u);
  argaddr(2, &sport_u);
  argaddr(3, &buf_u);
  argint(4, &maxlen);

  if(maxlen < 0)
    return -1;

  struct port_queue_mapping_entry *entry = 0;
  for(int i = 0; i < MAX_MAPPINGS; i++){
    acquire(&port_queue_mappings[i].lock);
    if(port_queue_mappings[i].port == dport){
      entry = &port_queue_mappings[i];
      break;
    }
    release(&port_queue_mappings[i].lock);
  }

  if(entry == 0)
    return -1;

  while(entry->size == 0)
    sleep(entry, &entry->lock);

  struct eth *eth = (struct eth *)pop_front(entry);
  release(&entry->lock);

  struct ip *ip = (struct ip *)(eth + 1);
  int ip_hlen = (ip->ip_vhl & 0x0f) * 4;
  struct udp *udp = (struct udp *)((char *)ip + ip_hlen);
  char *payload = (char *)(udp + 1);
  int udp_len = ntohs(udp->ulen);

  if(udp_len < (int)sizeof(*udp) || ip_hlen + udp_len > ntohs(ip->ip_len)){
    kfree((void *)eth);
    return -1;
  }

  uint32 srcaddr = ntohl(ip->ip_src);
  uint16 srcport = ntohs(udp->sport);
  int payloadlen = udp_len - sizeof(*udp);
  int copylen = payloadlen < maxlen ? payloadlen : maxlen;

  if(copyout(p->pagetable, srcaddr_u, (char *)&srcaddr, sizeof(srcaddr)) < 0 ||
     copyout(p->pagetable, sport_u, (char *)&srcport, sizeof(srcport)) < 0 ||
     copyout(p->pagetable, buf_u, payload, copylen) < 0){
    kfree((void *)eth);
    return -1;
  }

  kfree((void *)eth);
  return copylen;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  if(sport <= 0 || sport > 0xffff || dport <= 0 || dport > 0xffff || len < 0)
    return -1;

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0)
    return -1;
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *)buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45;
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl((uint32)dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons((uint16)sport);
  udp->dport = htons((uint16)dport);
  udp->ulen = htons(len + sizeof(*udp));
  udp->sum = 0;

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    return -1;
  }

  if(e1000_transmit(buf, total) < 0){
    kfree(buf);
    return -1;
  }
  return 0;
}

//
// tcplisten(port) -> listener id
//
uint64
sys_tcplisten(void)
{
  int port;
  argint(0, &port);

  if(port <= 0 || port > 0xffff)
    return -1;

  acquire(&tcplock);

  if(tcp_lport_busy_locked((uint16)port)){
    release(&tcplock);
    return -1;
  }

  struct tcp_conn *listener = tcp_alloc_conn_locked();
  if(listener == 0){
    release(&tcplock);
    return -1;
  }

  listener->state = TCP_STATE_LISTEN;
  listener->lport = (uint16)port;
  listener->parent = -1;

  int id = tcp_conn_id(listener);
  release(&tcplock);
  return id;
}

//
// tcpaccept(listener_id, *src, *sport) -> connection id
//
uint64
sys_tcpaccept(void)
{
  struct proc *p = myproc();
  int listener_id;
  uint64 src_u;
  uint64 sport_u;

  argint(0, &listener_id);
  argaddr(1, &src_u);
  argaddr(2, &sport_u);

  acquire(&tcplock);

  struct tcp_conn *listener = tcp_conn_from_id_locked(listener_id);
  if(listener == 0 || listener->state != TCP_STATE_LISTEN){
    release(&tcplock);
    return -1;
  }

  while(listener->acc_len == 0){
    sleep(listener, &tcplock);
    if(listener->used == 0 || listener->state != TCP_STATE_LISTEN){
      release(&tcplock);
      return -1;
    }
  }

  int conn_id = tcp_dequeue_accept_locked(listener);
  struct tcp_conn *conn = tcp_conn_from_id_locked(conn_id);
  if(conn == 0){
    release(&tcplock);
    return -1;
  }

  uint32 src = conn->rip;
  uint16 sport = conn->rport;
  if(copyout(p->pagetable, src_u, (char *)&src, sizeof(src)) < 0 ||
     copyout(p->pagetable, sport_u, (char *)&sport, sizeof(sport)) < 0){
    release(&tcplock);
    return -1;
  }

  release(&tcplock);
  return conn_id;
}

//
// tcpconnect(sport, dst_ip, dport) -> connection id
//
uint64
sys_tcpconnect(void)
{
  int sport;
  int dst;
  int dport;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);

  if(sport <= 0 || sport > 0xffff || dport <= 0 || dport > 0xffff)
    return -1;

  acquire(&tcplock);

  if(tcp_lport_busy_locked((uint16)sport)){
    release(&tcplock);
    return -1;
  }

  struct tcp_conn *conn = tcp_alloc_conn_locked();
  if(conn == 0){
    release(&tcplock);
    return -1;
  }

  conn->state = TCP_STATE_SYN_SENT;
  conn->parent = -1;
  conn->lport = (uint16)sport;
  conn->rip = (uint32)dst;
  conn->rport = (uint16)dport;

  conn->iss = tcp_iss;
  tcp_iss += 1024;
  if(tcp_iss == 0)
    tcp_iss = 1;

  conn->snd_una = conn->iss;
  conn->snd_nxt = conn->iss + 1;
  conn->rcv_nxt = 0;

  int ok = 0;
  for(int tries = 0; tries < TCP_MAX_RETRY && ok == 0; tries++){
    tcp_send_segment(conn->rip, conn->lport, conn->rport,
                     conn->iss, 0,
                     TCP_SYN, tcp_adv_window_locked(conn), 0, 0);

    for(int waited = 0; waited < TCP_RETX_TICKS; waited++){
      if(conn->used == 0 || conn->reset){
        release(&tcplock);
        return -1;
      }
      if(conn->state == TCP_STATE_ESTABLISHED){
        ok = 1;
        break;
      }
      sleep(&ticks, &tcplock);
    }
  }

  if(ok == 0){
    tcp_reset_conn(conn);
    release(&tcplock);
    return -1;
  }

  int id = tcp_conn_id(conn);
  release(&tcplock);
  return id;
}

//
// tcpsend(conn_id, buf, len)
//
uint64
sys_tcpsend(void)
{
  struct proc *p = myproc();
  int conn_id;
  uint64 buf_u;
  int len;

  argint(0, &conn_id);
  argaddr(1, &buf_u);
  argint(2, &len);

  if(len < 0)
    return -1;
  if(len == 0)
    return 0;

  acquire(&tcplock);

  struct tcp_conn *conn = tcp_conn_from_id_locked(conn_id);
  if(conn == 0 ||
     (conn->state != TCP_STATE_ESTABLISHED && conn->state != TCP_STATE_CLOSE_WAIT)){
    release(&tcplock);
    return -1;
  }

  int sent = 0;
  char payload[TCP_TX_CHUNK];

  while(sent < len){
    int n = len - sent;
    if(n > TCP_TX_CHUNK)
      n = TCP_TX_CHUNK;

    if(copyin(p->pagetable, payload, buf_u + sent, n) < 0){
      release(&tcplock);
      return sent > 0 ? sent : -1;
    }

    uint32 seq = conn->snd_nxt;
    uint32 target = conn->snd_nxt + n;
    conn->snd_nxt = target;

    int acked = 0;
    for(int tries = 0; tries < TCP_MAX_RETRY && acked == 0; tries++){
      tcp_send_segment(conn->rip, conn->lport, conn->rport,
                       seq, conn->rcv_nxt,
                       TCP_ACK | TCP_PSH, tcp_adv_window_locked(conn), payload, n);

      for(int waited = 0; waited < TCP_RETX_TICKS; waited++){
        if(conn->used == 0 || conn->reset || conn->state == TCP_STATE_CLOSED){
          release(&tcplock);
          return sent > 0 ? sent : -1;
        }
        if(seq_ge(conn->snd_una, target)){
          acked = 1;
          break;
        }
        sleep(&ticks, &tcplock);
      }
    }

    if(acked == 0){
      release(&tcplock);
      return sent > 0 ? sent : -1;
    }

    sent += n;
  }

  release(&tcplock);
  return sent;
}

//
// tcprecv(conn_id, buf, maxlen)
//
uint64
sys_tcprecv(void)
{
  struct proc *p = myproc();
  int conn_id;
  uint64 buf_u;
  int maxlen;

  argint(0, &conn_id);
  argaddr(1, &buf_u);
  argint(2, &maxlen);

  if(maxlen < 0)
    return -1;
  if(maxlen == 0)
    return 0;

  acquire(&tcplock);

  struct tcp_conn *conn = tcp_conn_from_id_locked(conn_id);
  if(conn == 0 || conn->state == TCP_STATE_LISTEN){
    release(&tcplock);
    return -1;
  }

  while(conn->rx_len == 0){
    if(conn->reset){
      release(&tcplock);
      return -1;
    }
    if(conn->peer_fin || conn->state == TCP_STATE_CLOSED){
      release(&tcplock);
      return 0;
    }
    sleep(conn, &tcplock);
  }

  int n = conn->rx_len < maxlen ? conn->rx_len : maxlen;
  int first = TCP_RX_BUF - conn->rx_head;
  if(first > n)
    first = n;

  if(copyout(p->pagetable, buf_u, conn->rx_buf + conn->rx_head, first) < 0){
    release(&tcplock);
    return -1;
  }

  int second = n - first;
  if(second > 0){
    if(copyout(p->pagetable, buf_u + first, conn->rx_buf, second) < 0){
      release(&tcplock);
      return -1;
    }
  }

  conn->rx_head = (conn->rx_head + n) % TCP_RX_BUF;
  conn->rx_len -= n;

  // If peer was blocked by our full window, advertise new room quickly.
  if(conn->state != TCP_STATE_CLOSED)
    tcp_send_ack_locked(conn);

  release(&tcplock);
  return n;
}

//
// tcpclose(conn_or_listener_id)
//
uint64
sys_tcpclose(void)
{
  int id;
  argint(0, &id);

  acquire(&tcplock);

  struct tcp_conn *conn = tcp_conn_from_id_locked(id);
  if(conn == 0){
    release(&tcplock);
    return -1;
  }

  if(conn->state == TCP_STATE_LISTEN){
    int lidx = (int)(conn - tcp_conns);

    // Tear down children created under this listener.
    for(int i = 0; i < TCP_MAX_CONN; i++){
      if(tcp_conns[i].used && tcp_conns[i].parent == lidx)
        tcp_close_with_rst_locked(&tcp_conns[i]);
    }

    tcp_reset_conn(conn);
    wakeup(conn);
    release(&tcplock);
    return 0;
  }

  if(conn->reset || conn->state == TCP_STATE_CLOSED){
    tcp_reset_conn(conn);
    release(&tcplock);
    return 0;
  }

  if(conn->state == TCP_STATE_SYN_SENT || conn->state == TCP_STATE_SYN_RCVD){
    tcp_close_with_rst_locked(conn);
    release(&tcplock);
    return 0;
  }

  if(conn->state == TCP_STATE_ESTABLISHED){
    uint32 finseq = conn->snd_nxt;
    conn->snd_nxt++;
    conn->state = TCP_STATE_FIN_WAIT_1;

    for(int tries = 0; tries < TCP_MAX_RETRY; tries++){
      tcp_send_segment(conn->rip, conn->lport, conn->rport,
                       finseq, conn->rcv_nxt,
                       TCP_FIN | TCP_ACK, tcp_adv_window_locked(conn), 0, 0);

      int done = 0;
      for(int waited = 0; waited < TCP_RETX_TICKS; waited++){
        if(conn->state != TCP_STATE_FIN_WAIT_1){
          done = 1;
          break;
        }
        sleep(&ticks, &tcplock);
      }
      if(done)
        break;
    }
  } else if(conn->state == TCP_STATE_CLOSE_WAIT){
    uint32 finseq = conn->snd_nxt;
    conn->snd_nxt++;
    conn->state = TCP_STATE_LAST_ACK;

    for(int tries = 0; tries < TCP_MAX_RETRY; tries++){
      tcp_send_segment(conn->rip, conn->lport, conn->rport,
                       finseq, conn->rcv_nxt,
                       TCP_FIN | TCP_ACK, tcp_adv_window_locked(conn), 0, 0);

      int done = 0;
      for(int waited = 0; waited < TCP_RETX_TICKS; waited++){
        if(conn->state != TCP_STATE_LAST_ACK){
          done = 1;
          break;
        }
        sleep(&ticks, &tcplock);
      }
      if(done)
        break;
    }
  }

  // Wait a bounded amount of time for final close state.
  for(int waited = 0; waited < TCP_RETX_TICKS * TCP_MAX_RETRY; waited++){
    if(conn->state == TCP_STATE_CLOSED || conn->reset)
      break;
    sleep(&ticks, &tcplock);
  }

  tcp_reset_conn(conn);
  release(&tcplock);
  return 0;
}

static void
udp_rx(char *buf, struct ip *ip, int ip_hlen, int ip_len)
{
  if(ip_len < ip_hlen + (int)sizeof(struct udp)){
    kfree(buf);
    return;
  }

  struct udp *udp = (struct udp *)((char *)ip + ip_hlen);
  int udp_len = ntohs(udp->ulen);
  if(udp_len < (int)sizeof(struct udp) || ip_hlen + udp_len > ip_len){
    kfree(buf);
    return;
  }
  uint16 port = ntohs(udp->dport);

  for(int i = 0; i < MAX_MAPPINGS; i++){
    acquire(&port_queue_mappings[i].lock);
    if(port_queue_mappings[i].port == port){
      if(push_back(&port_queue_mappings[i], buf) < 0){
        release(&port_queue_mappings[i].lock);
        kfree(buf);
        return;
      }
      wakeup(&port_queue_mappings[i]);
      release(&port_queue_mappings[i].lock);
      return;
    }
    release(&port_queue_mappings[i].lock);
  }

  // Port is not bound.
  kfree(buf);
}

static void
tcp_rx(char *buf, struct ip *ip, int ip_hlen, int ip_len)
{
  int tcp_len = ip_len - ip_hlen;
  if(tcp_len < (int)sizeof(struct tcp)){
    kfree(buf);
    return;
  }

  struct tcp *tcp = (struct tcp *)((char *)ip + ip_hlen);
  int tcp_hlen = ((tcp->off >> 4) & 0xf) * 4;
  if(tcp_hlen < (int)sizeof(struct tcp) || tcp_hlen > tcp_len){
    kfree(buf);
    return;
  }

  uint16 old_sum = tcp->sum;
  tcp->sum = 0;
  uint16 calc = tcp_checksum(ip, tcp, tcp_len);
  tcp->sum = old_sum;
  if(calc != ntohs(old_sum)){
    kfree(buf);
    return;
  }

  char *payload = (char *)tcp + tcp_hlen;
  int payloadlen = tcp_len - tcp_hlen;

  uint32 src_ip = ntohl(ip->ip_src);
  uint16 src_port = ntohs(tcp->sport);
  uint16 dst_port = ntohs(tcp->dport);

  acquire(&tcplock);

  struct tcp_conn *conn = tcp_find_conn_locked(src_ip, src_port, dst_port);
  if(conn){
    tcp_handle_segment_locked(conn, tcp, payload, payloadlen);
    release(&tcplock);
    kfree(buf);
    return;
  }

  // No exact connection: maybe this is a new SYN for a listener.
  if((tcp->flags & TCP_SYN) && !(tcp->flags & TCP_ACK) && !(tcp->flags & TCP_RST)){
    struct tcp_conn *listener = tcp_find_listener_locked(dst_port);
    if(listener){
      struct tcp_conn *child = tcp_alloc_conn_locked();
      if(child){
        child->state = TCP_STATE_SYN_RCVD;
        child->parent = (int)(listener - tcp_conns);
        child->lport = dst_port;
        child->rip = src_ip;
        child->rport = src_port;

        child->iss = tcp_iss;
        tcp_iss += 1024;
        if(tcp_iss == 0)
          tcp_iss = 1;

        child->snd_una = child->iss;
        child->snd_nxt = child->iss + 1;
        child->rcv_nxt = ntohl(tcp->seq) + 1;

        tcp_send_segment(child->rip, child->lport, child->rport,
                         child->iss, child->rcv_nxt,
                         TCP_SYN | TCP_ACK, tcp_adv_window_locked(child), 0, 0);
      }
      release(&tcplock);
      kfree(buf);
      return;
    }
  }

  // Unmatched segment: send RST as minimal RFC-compatible behavior.
  if((tcp->flags & TCP_RST) == 0)
    tcp_send_rst_for_unmatched(ip, tcp, payloadlen);

  release(&tcplock);
  kfree(buf);
}

void
ip_rx(char *buf, int len)
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  if(len < (int)(sizeof(struct eth) + sizeof(struct ip))){
    kfree(buf);
    return;
  }

  struct eth *eth = (struct eth *)buf;
  struct ip *ip = (struct ip *)(eth + 1);

  int version = (ip->ip_vhl >> 4) & 0xf;
  int ip_hlen = (ip->ip_vhl & 0xf) * 4;
  if(version != 4 || ip_hlen < (int)sizeof(struct ip)){
    kfree(buf);
    return;
  }

  int ip_len = ntohs(ip->ip_len);
  if(ip_len < ip_hlen || len < (int)sizeof(struct eth) + ip_len){
    kfree(buf);
    return;
  }

  // We only process packets destined to this xv6 instance.
  if(ntohl(ip->ip_dst) != local_ip){
    kfree(buf);
    return;
  }

  if(ip->ip_p == IPPROTO_UDP)
    udp_rx(buf, ip, ip_hlen, ip_len);
  else if(ip->ip_p == IPPROTO_TCP)
    tcp_rx(buf, ip, ip_hlen, ip_len);
  else
    kfree(buf);
}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *)inbuf;
  struct arp *inarp = (struct arp *)(ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");

  struct eth *eth = (struct eth *)buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));
  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *)buf;

  if(len >= (int)(sizeof(struct eth) + sizeof(struct arp)) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= (int)(sizeof(struct eth) + sizeof(struct ip)) &&
            ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
