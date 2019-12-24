struct job_ctx;
typedef struct job_ctx *ctx_t;
struct job_ctx {
  char *tgt;
  int seq;
  float interval;
  struct sockaddr_in addr;
  struct icmphdr icmp_hdr;
  struct timespec ts_tx;
  struct timespec ts_rx;
  uint64_t rtt_ns;
  ev_timer timeout;
  ev_io *io_r;
  ev_io *io_w;
  int sock;
  int loss;
  int loss_thr;
  char state;
  char last_state;
  uint8_t ipttl;
  ctx_t next;
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

ctx_t ctx_new(char *tgt, ev_io *io_r, ev_io *io_w, int sock);
void ctx_put(ctx_t c);
ctx_t ctx_lookup(in_addr_t addr);
void ctx_enqueue(ctx_t c);
ctx_t ctx_dequeue();

void ctx_handle_timeout(ctx_t c);
int ctx_handle_reply(ctx_t c, char *buf);
void ctx_make_request(ctx_t c, char *buf, int len);
void ctx_update_ts(ctx_t c, int tx, struct timespec *ts);
void ctx_write_log(ctx_t c);

