#include "pping.h"

static ctx_t *ctx_map[65536];
static inline uint64_t tsdiff(struct timespec t1, struct timespec t2) {
  uint64_t ts1 = t1.tv_sec * 1000000000L + t1.tv_nsec;
  uint64_t ts2 = t2.tv_sec * 1000000000L + t2.tv_nsec;
  return ts2 > ts1 ? ts2 - ts1 : 1;
};

void ctx_put(ctx_t c) {
  uint32_t uaddr = ntohl(c->addr.sin_addr.s_addr);
  uint32_t h = uaddr >> 16;
  uint32_t l = uaddr & 0xffff;
  ctx_t *lv2 = ctx_map[h];
  if (!lv2) {
    ctx_map[h] = lv2 = malloc(65536 * sizeof(ctx_t));
  }
  lv2[l] = c;
}

ctx_t ctx_lookup(in_addr_t addr) {
  uint32_t uaddr = ntohl(addr);
  uint32_t h = uaddr >> 16;
  uint32_t l = uaddr & 0xffff;
  ctx_t *lv2 = ctx_map[h];
  if (!lv2)
    return NULL;
  return lv2[l];
}

static ctx_t ctx_head = NULL, ctx_tail = NULL;
int ctx_qlen = 0;
void ctx_enqueue(ctx_t c) {
  if (c->next) {
    return;
  }
  if (ctx_tail) {
    ctx_tail->next = c;
    ctx_tail = c;
  } else {
    ctx_tail = ctx_head = c;
    c->next = NULL;
  }
  ctx_qlen++;
}

ctx_t ctx_dequeue() {
  if (ctx_head) {
    ctx_t c = ctx_head;
    if (c->next) {
      ctx_head = c->next;
    } else {
      ctx_head = ctx_tail = NULL;
    }
    c->next = NULL;
    ctx_qlen--;
    return c;
  } else {
    return NULL;
  }
}

ctx_t ctx_new(char *tgt, ev_io *io_r, ev_io *io_w, int sock) {
  ctx_t c = malloc(sizeof(struct job_ctx));
  memset(c, 0, sizeof(struct job_ctx));
  c->seq = 1;
  c->interval = 1.0;
  c->loss_thr = 10;

  c->addr.sin_family = AF_INET;
  if (inet_pton(AF_INET, tgt, &c->addr.sin_addr.s_addr) <= 0) {
    free(c);
    return NULL;
  }
  inet_ntop(AF_INET, &c->addr.sin_addr.s_addr, c->tgt, sizeof(c->tgt));
  strncpy(c->tgt, tgt, sizeof(c->tgt));
  c->icmp_hdr.type = ICMP_ECHO;
  c->icmp_hdr.un.echo.sequence = htons(1);

  c->timeout.data = c;
  c->state = c->last_state = JOB_STATE_INIT;

  c->io_w = io_w;
  c->io_r = io_r;
  c->sock = sock;

  ctx_put(c);
  // ctx_enqueue(c);

  return c;
}

void ctx_set_state(ctx_t c, char s) {
  c->last_state = c->state;
  c->state = s;
}

void ctx_handle_timeout(ctx_t c) {
  if (c->rtt_ns == 0) { // rtt_ns==0 means no reply received
    c->loss++;
    /*
    I timeout I
    U timeout L
    L timeout L
    L timeout loss>thr D
    D timeout D
    */
    switch (c->state) {
    case JOB_STATE_INIT:
      break;
    case JOB_STATE_UP:
      ctx_set_state(c, JOB_STATE_LOSS);
      fprintf(stderr, "LOSS:%s\n", c->tgt);
      break;
    case JOB_STATE_LOSS:
      if (c->loss > c->loss_thr) {
        ctx_set_state(c, JOB_STATE_DOWN);
        fprintf(stderr, "DOWN:%s\n", c->tgt);
      }
      break;
    case JOB_STATE_DOWN:
      break;
    }
  }
  if (c->state == JOB_STATE_DOWN || c->state == JOB_STATE_INIT) {
    // liner backoff, with randomization, max timeout 60s
    float d = (c->loss - c->loss_thr) * 1.5;
    if (d > 60.0)
      d = 60.0;
    d += rand() % 10 / 10.0;
    ev_timer_set(&c->timeout, d, d);
    // printf("loss=%f\n",d);
  }
  ctx_enqueue(c);
}

static uint16_t icmp_csum(uint16_t *icmph, int len) {
  assert(len >= 0);

  uint16_t ret = 0;
  uint32_t sum = 0;
  uint16_t odd_byte;

  while (len > 1) {
    sum += *icmph++;
    len -= 2;
  }

  if (len == 1) {
    *(uint8_t *)(&odd_byte) = *(uint8_t *)icmph;
    sum += odd_byte;
  }

  sum = (sum >> 16) + (sum & 0xffff);
  sum += (sum >> 16);
  ret = ~sum;

  return ret;
}

void ctx_make_request(ctx_t c, char *buf, int len) {
  c->rtt_ns = 0;
  c->icmp_hdr.un.echo.sequence = htons(c->seq++);
  c->icmp_hdr.checksum = 0;
  memcpy(buf, &c->icmp_hdr, sizeof(c->icmp_hdr));
  c->icmp_hdr.checksum = icmp_csum(buf, len);
  memcpy(buf, &c->icmp_hdr, sizeof(c->icmp_hdr));
}

void ctx_update_ts(ctx_t c, int tx, struct timespec *ts) {
  struct timespec *tgt = (tx == CTX_TS_TX) ? &c->ts_tx : &c->ts_rx;
  if (ts) {
    memcpy(tgt, ts, sizeof(c->ts_rx));
  } else {
    clock_gettime(CLOCK_REALTIME, tgt);
  }
}

void ctx_write_log(ctx_t c) {
  if (c->state == JOB_STATE_UP) {
    uint64_t t = c->rtt_ns = tsdiff(c->ts_tx, c->ts_rx);
    log_write("%d,%s,%lf,%d,%f,%c,%c,%u\n", c->ts_tx.tv_sec, c->tgt,
              t / 1000000.0, c->loss, c->timeout.repeat, c->last_state,
              c->state, c->ipttl);
  } else if (c->state != JOB_STATE_INIT) {
    log_write("%d,%s,-1,%d,%f,%c,%c,%u\n", c->ts_tx.tv_sec, c->tgt, c->loss,
              c->timeout.repeat, c->last_state, c->state, c->ipttl);
  }
}

int ctx_handle_reply(ctx_t c, char *buf) {
  uint8_t iplen = (0x0f & (*(uint8_t *)buf)) * 4;
  c->ipttl = buf[8];
  struct icmphdr *icmp_hdr = (void *)buf + iplen;

  if (icmp_hdr->type != ICMP_ECHOREPLY)
    return -1;
  if (icmp_hdr->un.echo.sequence != c->icmp_hdr.un.echo.sequence) {
    // fprintf(stderr, "seq mismatch: %d %d\n", icmp_hdr->un.echo.sequence,
    //         htons(c->icmp_hdr.un.echo.sequence));
    // fprintf(stderr,
    //         "ICMP header: \n"
    //         "Type: %d, "
    //         "Code: %d, ID: %d, Sequence: %d\n",
    //         icmp_hdr->type, icmp_hdr->code, ntohs(icmp_hdr->un.echo.id),
    //         ntohs(icmp_hdr->un.echo.sequence));
    // fprintf(stderr,
    //         "ICMP header: \n"
    //         "Type: %d, "
    //         "Code: %d, ID: %d, Sequence: %d\n",
    //         c->icmp_hdr.type, c->icmp_hdr.code,
    //         ntohs(c->icmp_hdr.un.echo.id),
    //         ntohs(c->icmp_hdr.un.echo.sequence));
    return -2;
  }
  if (icmp_hdr->un.echo.id != c->icmp_hdr.un.echo.id) {
    return -3;
  }
  /*
  I reply U
  U reply U
  D reply U
  L reply U
  */
  ctx_set_state(c, JOB_STATE_UP);
  switch (c->state) {
  case JOB_STATE_LOSS:
  case JOB_STATE_DOWN:
    fprintf(stderr, "UP:%s\n", c->tgt);
    break;
  }
  c->loss = 0;
  //   clock_gettime(CLOCK_REALTIME, &c->ts_rx);
  ev_timer_set(&c->timeout, c->interval, c->interval);
  return 0;
}
