struct job_ctx;
typedef struct job_ctx* ctx_t;
struct job_ctx {
    char* tgt;
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
    ctx_t next;
};

#define JOB_STATE_INIT 'I'
#define JOB_STATE_UP   'U'
#define JOB_STATE_LOSS 'L'
#define JOB_STATE_DOWN 'D'

static const char* job_state_name[] = {"init","up","loss","down"};
extern int ctx_qlen;

ctx_t ctx_new(char *tgt, ev_io *io_r, ev_io *io_w, int sock);
void ctx_put(ctx_t c);
ctx_t ctx_lookup(in_addr_t addr);
void ctx_enqueue(ctx_t c);
ctx_t ctx_dequeue();

void ctx_handle_timeout(ctx_t c);
void ctx_handle_reply(ctx_t c,char* buf);
void ctx_make_request(ctx_t c,char* buf);

static inline uint64_t tsdiff(struct timespec t1, struct timespec t2){
    struct timespec diff;
    if (t2.tv_nsec-t1.tv_nsec < 0) {
        diff.tv_sec  = t2.tv_sec - t1.tv_sec - 1;
        diff.tv_nsec = t2.tv_nsec - t1.tv_nsec + 1000000000;
    } else {
        diff.tv_sec  = t2.tv_sec - t1.tv_sec;
        diff.tv_nsec = t2.tv_nsec - t1.tv_nsec;
    }
    return (diff.tv_sec * 1000000000L + diff.tv_nsec);
};

