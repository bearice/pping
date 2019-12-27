#include <sys/queue.h>

struct ctx;
typedef struct ctx *ctx_t;

struct ctx_io;

struct ctx {
  char tgt[32];
  struct sockaddr_in addr;
  struct icmphdr icmp_hdr;
  int seq;
  uint8_t ip_ttl;

  struct io_ctx *io;

  struct timespec ts_tx;
  struct timespec ts_rx;
  uint64_t rtt_ns;

  float interval;
  ev_timer timeout;
  int reload_cnt;

  int loss;
  int loss_thr;
  char state;
  char last_state;

  ctx_t q_prev;
  ctx_t q_next;
  ctx_t l_prev;
  ctx_t l_next;
};

/*
state transit:
I reply U
U reply U
D reply U
L reply U

I timeout I
U timeout L
L timeout L
L timeout loss>thr D
D timeout D
*/
#define JOB_STATE_INIT 'I'
#define JOB_STATE_UP 'U'
#define JOB_STATE_LOSS 'L'
#define JOB_STATE_DOWN 'D'

#define CTX_TS_TX 1
#define CTX_TS_RX 0

// static const char *job_state_name[] = {"init", "up", "loss", "down"};
extern int ctx_qlen;
extern ctx_t ctx_lhead;

ctx_t ctx_new(char *tgt, struct io_ctx *);
void ctx_free(ctx_t ctx);
// int ctx_gc(int);
ctx_t ctx_lookup(in_addr_t addr);
void ctx_enqueue(ctx_t c);
ctx_t ctx_dequeue(void);

void ctx_handle_timeout(ctx_t c);
int ctx_handle_reply(ctx_t c, uint8_t *buf);
void ctx_make_request(ctx_t c, uint8_t *buf, int len);
void ctx_update_ts(ctx_t c, int tx, struct timespec *ts);
void ctx_write_log(ctx_t c);
