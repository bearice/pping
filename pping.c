#include "pping.h"

struct timespec *find_ts(struct msghdr *msg) {
  struct timespec *ts = NULL;
  struct cmsghdr *cmsg;
  //   struct sock_extended_err *ext;

  for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
    // printf("level=%d, type=%d, len=%zu\n", cmsg->cmsg_level, cmsg->cmsg_type,
    //        cmsg->cmsg_len);

    // if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR) {
    //   ext = (struct sock_extended_err *)CMSG_DATA(cmsg);
    //   printf("errno=%d, origin=%d\n", ext->ee_errno, ext->ee_origin);
    //   continue;
    // }

    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING)
      ts = (struct timespec *)CMSG_DATA(cmsg);
  }
  if (ts && ts[2].tv_sec) {
    puts("using hw timestamp");
    return ts + 2;
  } else {
    return ts;
  }
}

int rx_cnt = 0, tx_cnt = 0, to_cnt = 0, eq_cnt = 0, wt_cnt = 0, we_cnt = 0,
    bo_cnt = 0;

static void read_cb(EV_P_ ev_io *w, int revents) {
  rx_cnt++;
  unsigned char buf[2048];
  char ctlbuf[4096];
  struct sockaddr_in addr = {0};
  socklen_t slen = sizeof(addr);
  struct iovec iov = {.iov_base = buf, .iov_len = sizeof(buf)};
  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_name = &addr;
  msg.msg_namelen = sizeof(addr);
  msg.msg_control = ctlbuf;
  msg.msg_controllen = sizeof(ctlbuf);
  int ret = recvmsg(w->fd, &msg, 0);
  if (ret < 0) {
    perror("recvfrom");
    return;
  }
  ctx_t c = ctx_lookup(addr.sin_addr.s_addr);
  //   printf("read c=%x\n", c);
  if (!c) {
    return;
  }
  struct timespec *ts = find_ts(&msg);
  ctx_update_ts(c, CTX_TS_RX, ts);
  ctx_handle_reply(c, buf);
  ctx_write_log(c);
  //   printf("repeat=%f\n", c->timeout.repeat);
  ev_timer_again(EV_A_ & c->timeout);
}

static void write_cb(EV_P_ ev_io *w, int revents) {
  ctx_t c = ctx_dequeue();
  //   printf("write c=%x\n", c);
  if (c) {
    tx_cnt++;
    if (c->next)
      wt_cnt++;
    char buf[128];
    char ctlbuf[4096];
    ctx_make_request(c, buf);
    struct iovec iov = {.iov_base = buf, .iov_len = sizeof(buf)};
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_name = &c->addr;
    msg.msg_namelen = sizeof(c->addr);
    msg.msg_control = ctlbuf;
    msg.msg_controllen = 0;
    int ret = sendmsg(c->sock, &msg, 0);
    if (ret < 0) {
      we_cnt++;
      return;
    }

    msg.msg_controllen = sizeof(ctlbuf);
    ret = recvmsg(c->sock, &msg, MSG_ERRQUEUE);
    if (ret < 0) {
      we_cnt++;
      return;
    }

    struct timespec *ts = find_ts(&msg);
    ctx_update_ts(c, CTX_TS_TX, ts);
    // ctx_write_log(c);
  } else {
    eq_cnt++;
    ev_io_stop(EV_A_ w);
  }
}

static void timeout_cb(EV_P_ ev_timer *w, int revents) {
  to_cnt++;
  ctx_t c = (ctx_t)w->data;
  char st = c->state;
  //   printf("timeout c=%x st=%c\n", c, st);
  ctx_update_ts(c, CTX_TS_TX, NULL);
  ctx_handle_timeout(c);
  if (c->rtt_ns == 0)
    ctx_write_log(c);
  ev_timer_again(EV_A_ & c->timeout);
  ev_io_start(EV_A_ c->io_w);
}

static void stat_cb(EV_P_ ev_timer *w, int revents) {
  fprintf(stderr, "TX=%d RX=%d TO=%d EQ=%d WT=%d WE=%d BO=%d Q=%d\n", tx_cnt,
          rx_cnt, to_cnt, eq_cnt, wt_cnt, we_cnt, bo_cnt, ctx_qlen);
  tx_cnt = rx_cnt = to_cnt = eq_cnt = wt_cnt = we_cnt = 0;
  ev_timer_again(EV_A_ w);
}

int main(int argc, char **argv) {
  int opt;
  char *log_name = NULL;
  int log_len = 1000 * 1000 * 10;
  while ((opt = getopt(argc, argv, "o:l:")) != -1) {
    switch (opt) {
    case 'o':
      log_name = optarg;
      break;
    case 'l':
      log_len = atoi(optarg);
      break;
    default: /* '?' */
      fprintf(stderr, "Usage: %s [-o log_name] [-l log_name] targets...\n",
              argv[0]);
      exit(EXIT_FAILURE);
    }
  }

  int i, flags;
  struct ev_loop *loop;
  int sock_cnt = 1;
  int *sock = malloc(sizeof(int) * sock_cnt);
  ev_io *io_r = malloc(sizeof(ev_io) * sock_cnt);
  ev_io *io_w = malloc(sizeof(ev_io) * sock_cnt);
  ev_timer timer_stat;

  srand(time(NULL));
  log_setup(log_name, log_len);

  loop = EV_DEFAULT;
  for (i = 0; i < sock_cnt; i++) {
    sock[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (sock[i] < 0) {
      perror("socket()");
      return EXIT_FAILURE;
    }
    flags = fcntl(sock[i], F_GETFL);
    fcntl(sock[i], F_SETFL, flags | O_NONBLOCK);

    int val = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE |
              SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_TX_SOFTWARE;
    val |= SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
    setsockopt(sock[i], SOL_SOCKET, SO_TIMESTAMPING, &val, sizeof(val));

    ev_io_init(io_r + i, read_cb, sock[i], EV_READ);
    ev_io_init(io_w + i, write_cb, sock[i], EV_WRITE);
    ev_set_priority(io_r + i, 2);
    ev_set_priority(io_w + i, 1);
    ev_io_start(loop, io_r + i);
    ev_io_start(loop, io_w + i);
  }

  for (i = optind; i < argc; i++) {
    ctx_t c = ctx_new(argv[i], &io_r[i % sock_cnt], &io_w[i % sock_cnt],
                      sock[i % sock_cnt]);
    ev_timer_init(&c->timeout, timeout_cb, (i % 1000) * 0.001, c->interval);
    ev_timer_start(loop, &c->timeout);
  }

  ev_timer_init(&timer_stat, stat_cb, 1., 1.);
  ev_timer_start(loop, &timer_stat);

  ev_run(loop, 0);
  return 0;
}
