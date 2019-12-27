#include "pping.h"

#include <getopt.h>

struct io_ctx {
  int sock;
  ev_io r;
  ev_io w;
};

static struct timespec *find_ts(struct msghdr *msg) {
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

    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
      ts = (struct timespec *)CMSG_DATA(cmsg);
      if (ts[2].tv_sec)
        ts = ts + 2;
    } else if (cmsg->cmsg_level == SOL_SOCKET &&
               cmsg->cmsg_type == SO_TIMESTAMPNS)
      ts = (struct timespec *)CMSG_DATA(cmsg);
  }
  // puts("using hw timestamp");
  return ts;
}

int rx_cnt = 0, tx_cnt = 0, to_cnt = 0, eq_cnt = 0, wt_cnt = 0, we_cnt = 0,
    re_1 = 0, re_2 = 0, re_3 = 0, re_cnt = 0, bo_cnt = 0;

static void read_cb(EV_P_ ev_io *w, int revents) {
  rx_cnt++;
  unsigned char buf[2048];
  char ctlbuf[4096];
  struct sockaddr_in addr = {0};
  // socklen_t slen = sizeof(addr);
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
    re_cnt++;
    re_1++;
    // fprintf(stderr, "events=%d\n", revents);
    // exit(1);
    // perror("recvfrom");
    return;
  }
  ctx_t c = ctx_lookup(addr.sin_addr.s_addr);
  //   printf("read c=%x\n", c);
  if (!c) {
    re_2++;
    re_cnt++;
    return;
  }
  struct timespec *ts = find_ts(&msg);
  ctx_update_ts(c, CTX_TS_RX, ts);
  ret = ctx_handle_reply(c, buf);
  if (ret == 0)
    ctx_write_log(c);
  else {
    re_3++;
    re_cnt++;
  }
  //   printf("repeat=%f\n", c->timeout.repeat);
  ev_timer_again(EV_A_ & c->timeout);
}

static void write_cb(EV_P_ ev_io *w, int revents) {
  ctx_t c = ctx_dequeue();
  //   printf("write c=%x\n", c);
  if (c) {
    tx_cnt++;
    if (c->q_next)
      wt_cnt++;
    uint8_t buf[128];
    char ctlbuf[4096];
    ctx_make_request(c, buf, sizeof(buf));
    struct iovec iov = {.iov_base = buf, .iov_len = sizeof(buf)};
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_name = &c->addr;
    msg.msg_namelen = sizeof(c->addr);
    msg.msg_control = ctlbuf;
    msg.msg_controllen = 0;
    int ret = sendmsg(c->io->sock, &msg, 0);
    if (ret < 0) {
      we_cnt++;
      return;
    }

    // msg.msg_controllen = sizeof(ctlbuf);
    // ret = recvmsg(c->sock, &msg, MSG_ERRQUEUE);
    // if (ret < 0) {
    //   we_cnt++;
    //   return;
    // }

    // struct timespec *ts = find_ts(&msg);
    // ctx_update_ts(c, CTX_TS_TX, ts);
    ctx_update_ts(c, CTX_TS_TX, NULL);
    // ctx_write_log(c);
  } else {
    eq_cnt++;
    ev_io_stop(EV_A_ w);
  }
}

static void timeout_cb(EV_P_ ev_timer *w, int revents) {
  to_cnt++;
  ctx_t c = (ctx_t)w->data;
  // char st = c->state;
  //   printf("timeout c=%x st=%c\n", c, st);
  ctx_update_ts(c, CTX_TS_TX, NULL);
  ctx_handle_timeout(c);
  if (c->rtt_ns == 0)
    ctx_write_log(c);
  ev_timer_again(EV_A_ & c->timeout);
  ev_io_start(EV_A_ & c->io->w);
}

int verbose = 0;
static void stat_cb(EV_P_ ev_timer *w, int revents) {
  if (verbose)
    fprintf(stderr,
            "TX=%d RX=%d TO=%d EQ=%d WT=%d WE=%d RE=%d (%d %d %d) BO=%d Q=%d\n",
            tx_cnt, rx_cnt - re_cnt, to_cnt, eq_cnt, wt_cnt, we_cnt, re_cnt,
            re_1, re_2, re_3, bo_cnt, ctx_qlen);
  tx_cnt = rx_cnt = to_cnt = eq_cnt = wt_cnt = re_cnt = re_1 = re_2 = re_3 =
      we_cnt = 0;
  ev_timer_again(EV_A_ w);
}

static float interval = 1.0;
static float slowstart = 1.0;
static int loss_thr = 10;
static int reload_cnt = 0;
static int tgt_argc;
static char **tgt_argv;
static struct io_ctx *tgt_io;
static struct ev_loop *tgt_loop;

static int load_target_list() {
  reload_cnt++;
  int host_cnt = 0;
  char *tgt = NULL;
  FILE *f = NULL;
  char buf[32];
  int i = 0;
  while (i < tgt_argc) {
    // printf("f=%x i=%d\n", f, i);
    if (f == NULL && tgt_argv[i][0] == '@') {
      char *fn = &tgt_argv[i][1];
      f = fopen(fn, "r");
      // printf("f=%x i=%d s=%s\n", f, i, fn);
      if (!f)
        perror(fn);
    }
    if (f) {
      tgt = fgets(buf, sizeof(buf), f);
      if (NULL == tgt) {
        fclose(f);
        f = NULL;
        i++;
        continue;
      }
      tgt[strlen(tgt) - 1] = 0;
    } else {
      tgt = tgt_argv[i];
      i++;
    }
    ctx_t c = ctx_new(tgt, tgt_io);
    if (NULL == c) {
      fprintf(stderr, "Invalid ip address: %s\n", tgt);
      continue;
    }
    c->interval = interval;
    c->loss_thr = loss_thr;
    if (0 == c->reload_cnt) {
      float initial_delay = (host_cnt * slowstart / 1000.0);
      // printf("%s %f\n", c->tgt, initial_delay);
      ev_timer_init(&c->timeout, timeout_cb, initial_delay, c->interval);
      ev_timer_start(tgt_loop, &c->timeout);
    }
    c->reload_cnt = reload_cnt;
    host_cnt++;
  }

  if (reload_cnt > 1) {
    int n = 0;
    ctx_t c = ctx_lhead;
    while (c) {
      printf("c=%x rc=%d rcc=%d\n", c, c->reload_cnt, reload_cnt);
      if (c->reload_cnt < reload_cnt) {
        ctx_t next = c->l_next;
        ev_timer_stop(tgt_loop, &c->timeout);
        ctx_free(c);
        c = next;
        n++;
      } else {
        c = c->l_next;
      }
    }
    if (n)
      fprintf(stderr, "Removed %d targets\n", n);
  }
  return host_cnt;
}

static void sigusr1_cb(EV_P_ ev_signal *w, int revents) {
  int host_cnt = load_target_list();
  fprintf(stderr, "Loaded %d targets.\n", host_cnt);
}

static void sigint_cb(EV_P_ ev_signal *w, int revents) {
  ev_break(EV_A, EVBREAK_ALL);
}

int main(int argc, char **argv) {
  int opt;
  char *log_name = NULL;
  int log_len = 1000 * 1000 * 10;

  while ((opt = getopt(argc, argv, "vi:o:l:s:t:")) != -1) {
    switch (opt) {
    case 'v':
      verbose = 1;
      break;
    case 'o':
      log_name = optarg;
      break;
    case 'l':
      log_len = atoi(optarg);
      break;
    case 't':
      loss_thr = atoi(optarg);
      break;
    case 'i':
      interval = atof(optarg);
      if (interval <= 0) {
        fprintf(stderr, "bad interval: %s\n", optarg);
        exit(EXIT_FAILURE);
      }
      break;
    case 's':
      slowstart = atof(optarg);
      if (interval < 0) {
        fprintf(stderr, "bad slowstart: %s\n", optarg);
        exit(EXIT_FAILURE);
      }
      break;
    default: /* '?' */
      fprintf(stderr,
              "Usage: %s -v [-o log_name] [-l log_length] [-s slow_start]"
              " [-t loss_thr]"
              " [@target_file]"
              " targets...\n",
              argv[0]);
      exit(EXIT_FAILURE);
    }
  }
  int i, flags;

  // multi socket support is leaved here as is, but it did not improvement, so
  // it's hardcoded to 1
  int sock_cnt = 1;
  struct ev_loop *loop = EV_DEFAULT;

  srand(time(NULL));
  log_setup(log_name, log_len);
  struct io_ctx *io = malloc(sizeof(struct io_ctx) * sock_cnt);

  for (i = 0; i < sock_cnt; i++) {
    io[i].sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (io[i].sock < 0) {
      perror("socket()");
      return EXIT_FAILURE;
    }
    flags = fcntl(io[i].sock, F_GETFL);
    fcntl(io[i].sock, F_SETFL, flags | O_NONBLOCK);

    // TODO: use an cmd options to select timestamp method.
    // flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE |
    //           SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_TX_SOFTWARE;
    // flags |= SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
    flags = 1;
    // setsockopt(sock[i], SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags));
    setsockopt(io[i].sock, SOL_SOCKET, SO_TIMESTAMPNS, &flags, sizeof(flags));

    ev_io_init(&io[i].r, read_cb, io[i].sock, EV_READ);
    ev_io_init(&io[i].w, write_cb, io[i].sock, EV_WRITE);

    ev_io_start(loop, &io[i].r);
    ev_io_start(loop, &io[i].w);
  }
  tgt_argc = argc - optind;
  tgt_argv = argv + optind;
  tgt_io = io;
  tgt_loop = loop;

  int host_cnt = load_target_list();
  fprintf(stderr, "Loaded %d targets.\n", host_cnt);

  if (host_cnt == 0) {
    fputs("Nothing to do.", stderr);
    return 0;
  }

  ev_timer timer_stat;
  ev_timer_init(&timer_stat, stat_cb, 1., 1.);
  ev_timer_start(loop, &timer_stat);

  ev_signal sigint;
  ev_signal_init(&sigint, sigint_cb, SIGINT);
  ev_signal_start(loop, &sigint);

  ev_signal sigusr1;
  ev_signal_init(&sigusr1, sigusr1_cb, SIGUSR1);
  ev_signal_start(loop, &sigusr1);

  ev_run(loop, 0);
  return 0;
}
