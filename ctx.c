#include "pping.h"

static ctx_t *ctx_map[65536];

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
  c->tgt = tgt;
  c->seq = 1;
  c->interval = 1.0;
  c->loss_thr = 10;

  c->addr.sin_family = AF_INET;
  c->addr.sin_addr.s_addr = inet_addr(c->tgt);

  c->icmp_hdr.type = ICMP_ECHO;
  c->icmp_hdr.un.echo.sequence = htons(1);

  c->timeout.data = c;
  c->state = c->last_state = JOB_STATE_DOWN;

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
  if (c->rtt_ns == 0) {
    c->loss++;
    if (c->loss > c->loss_thr) {
      ctx_set_state(c, JOB_STATE_DOWN);
      if (c->last_state != JOB_STATE_DOWN) {
        fprintf(stderr, "DOWN:%s\n", c->tgt);
        // clock_gettime(CLOCK_REALTIME, &c->ts_tx);
      }
    } else if (c->last_state != JOB_STATE_DOWN) {
      ctx_set_state(c, JOB_STATE_LOSS);
      fprintf(stderr, "LOSS:%s\n", c->tgt);
      //   clock_gettime(CLOCK_REALTIME, &c->ts_tx);
    }
  }
  /*
  if(c->loss>3){
      float d = (c->loss-3)*2;
      if(d>600.0)d=600.0;
      d+=rand()%100/100.0;
      ev_timer_set(&c->timeout,d,d);
      //printf("loss=%f\n",d);
  }
  */
  ctx_enqueue(c);
}

void ctx_make_request(ctx_t c, char *buf) {
  c->rtt_ns = 0;
  c->icmp_hdr.un.echo.sequence = htons(c->seq++);
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
    log_write("%d,%s,%lf,%d,%f,%c,%c\n", c->ts_tx.tv_sec, c->tgt, t / 1000000.0,
              c->loss, c->timeout.repeat, c->last_state, c->state);
  } else if (c->last_state != JOB_STATE_DOWN) {
    log_write("%d,%s,-1,%d,%f,%c,%c\n", c->ts_tx.tv_sec, c->tgt, c->loss,
              c->timeout.repeat, c->last_state, c->state);
  }
}

void ctx_handle_reply(ctx_t c, char *buf) {
  struct icmphdr *icmp_hdr = (void *)buf;
  if (icmp_hdr->un.echo.sequence != c->icmp_hdr.un.echo.sequence) {
    fprintf(stderr, "seq mismatch: %d %d", icmp_hdr->un.echo.sequence,
            c->icmp_hdr.un.echo.sequence);
    return;
  }
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
}
