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
  job_state_e state;      // Current state of the job
  job_state_e last_state; // Previous state of the job

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
// #define JOB_STATE_INIT 'I' // Replaced by job_state_e enum
// #define JOB_STATE_UP 'U'   // Replaced by job_state_e enum
// #define JOB_STATE_LOSS 'L' // Replaced by job_state_e enum
// #define JOB_STATE_DOWN 'D' // Replaced by job_state_e enum

typedef enum {
  STATE_INIT, // Initial state, before any replies or definitive timeouts
  STATE_UP,   // Target is responsive, replies are being received
  STATE_LOSS, // Target is experiencing intermittent packet loss
  STATE_DOWN  // Target is unresponsive, loss threshold exceeded
} job_state_e;

#define CTX_TS_TX 1
#define CTX_TS_RX 0

// static const char *job_state_name[] = {"init", "up", "loss", "down"};
// extern int ctx_qlen; // Replaced by accessor get_ctx_qlen()
// extern ctx_t ctx_lhead; // Replaced by accessor get_ctx_lhead()

/**
 * @brief Creates a new ping context or retrieves an existing one for the target
 * IP.
 * @param tgt Target IP address string.
 * @param io Pointer to the I/O context (socket information).
 * @return Pointer to the context, or NULL on error.
 */
ctx_t ctx_new(char *tgt, struct io_ctx *);

/**
 * @brief Frees resources associated with a ping context and removes it from
 * management.
 * @param ctx The context to free.
 */
void ctx_free(ctx_t ctx);

// int ctx_gc(int); // Garbage collection for contexts (not implemented/used
// currently).

/**
 * @brief Looks up an existing context by its IP address (network byte order).
 * @param addr The IP address (in_addr_t) to look up.
 * @return Pointer to the context if found, otherwise NULL.
 */
ctx_t ctx_lookup(in_addr_t addr);

/**
 * @brief Adds a context to the send queue if not already present.
 * @param c The context to enqueue.
 */
void ctx_enqueue(ctx_t c);

/**
 * @brief Removes and returns a context from the head of the send queue.
 * @return Pointer to the dequeued context, or NULL if the queue is empty.
 */
ctx_t ctx_dequeue(void);

/**
 * @brief Handles a timeout event for a specific context (e.g., no ICMP reply
 * received).
 * @param c The context that timed out.
 */
void ctx_handle_timeout(ctx_t c);

/**
 * @brief Handles an ICMP reply received for a specific context.
 * @param c The context to which the reply belongs.
 * @param buf Buffer containing the received packet.
 * @return 0 on success, negative on error or invalid packet.
 */
int ctx_handle_reply(ctx_t c, uint8_t *buf);

/**
 * @brief Prepares an ICMP echo request packet in the provided buffer.
 * @param c The context for which the request is made.
 * @param buf Buffer to store the constructed packet.
 * @param len Length of the buffer (used for checksum, typically size of
 * icmphdr).
 */
void ctx_make_request(ctx_t c, uint8_t *buf, int len);

/**
 * @brief Updates transmit (TX) or receive (RX) timestamp for a context.
 * @param c The context to update.
 * @param tx Flag: CTX_TS_TX for transmit, CTX_TS_RX for receive.
 * @param ts Pointer to timespec; if NULL, current time is used.
 */
void ctx_update_ts(ctx_t c, int tx, struct timespec *ts);

/**
 * @brief Writes a log entry for the context's current state/event.
 * @param c The context to log.
 */
void ctx_write_log(ctx_t c);

// Accessor functions for variables previously managed globally by ctx.c
/**
 * @brief Returns the current number of items in the send queue.
 * @return Number of items in the send queue.
 */
int get_ctx_qlen(void);

/**
 * @brief Returns the head of the global list of all contexts.
 * This list allows iterating over all managed targets.
 * @return Pointer to the first context in the list, or NULL if empty.
 */
ctx_t get_ctx_lhead(void);
