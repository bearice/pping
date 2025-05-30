#include "pping.h"

// #include <linux/time.h>

// Named Constants for Magic Numbers
#define CTX_MAP_SIZE 0x10000
static const long NANOSECONDS_PER_SECOND = 1000000000L;
static const int MILLISECONDS_PER_SECOND = 1000;
static const int NANOSECONDS_PER_MILLISECOND = 1000000;
static const double DEFAULT_BACKOFF_FACTOR = 1.5;
static const double MAX_BACKOFF_INTERVAL_SEC = 60.0;
static const int BACKOFF_RANDOM_MODULO = 10;
static const double BACKOFF_RANDOM_DIVISOR = 10.0;
#define IP_IHL_MASK 0x0F
static const int IP_HEADER_LENGTH_MULTIPLIER = 4;
static const int IP_TTL_OFFSET = 8;
static const int DEFAULT_CTX_SEQ = 1;
static const double DEFAULT_CTX_INTERVAL = 1.0;
static const int DEFAULT_CTX_LOSS_THR = 10;
static const int MIN_BACKOFF_DELAY_IF_LOSS_BELOW_THRESHOLD = 1;
static const double MIN_EFFECTIVE_BACKOFF_DELAY = 1.0;
static const uint64_t MIN_TS_DIFF_RESULT = 1;

// Define the new ctx_manager structure
/**
 * @brief Manages all contexts (ping targets).
 *
 * This structure holds the primary data structures for managing and accessing
 * ping contexts:
 * - A two-level hash map (`ctx_map`) for quick context lookup by IP address.
 * - A send queue (`ctx_qhead`, `ctx_qtail`) for contexts ready for an ICMP
 * request.
 * - A global list (`ctx_lhead`, `ctx_ltail`) of all active contexts for
 * sequential access.
 * - A counter for the number of items in the send queue (`ctx_qlen`).
 */
struct ctx_manager {
  ctx_t *ctx_map[CTX_MAP_SIZE]; /**< Two-level hash map for contexts by IP
                                   address. First level indexed by upper 16 bits
                                   of IP, second by lower 16 bits. */
  ctx_t
      ctx_qhead; /**< Head of the send queue (contexts waiting to be pinged). */
  ctx_t ctx_qtail; /**< Tail of the send queue. */
  int ctx_qlen;
  ctx_t ctx_lhead;
  ctx_t ctx_ltail;
};

// Instantiate the ctx_manager globally and statically
static struct ctx_manager g_ctx_manager = {
    .ctx_map = {NULL}, // Initialize array of pointers to NULL
    .ctx_qhead = NULL,
    .ctx_qtail = NULL,
    .ctx_qlen = 0,
    .ctx_lhead = NULL,
    .ctx_ltail = NULL};

static void ctx_add(ctx_t c) {
  uint32_t uaddr = ntohl(c->addr.sin_addr.s_addr);
  uint32_t h = uaddr >> 16;    // Upper 16 bits for the first level index.
  uint32_t l = uaddr & 0xffff; // Lower 16 bits for the second level index.
  ctx_t *lv2 = g_ctx_manager.ctx_map[h];

  // Allocate the second level of the map if it doesn't exist for this /16
  // prefix.
  if (!lv2) {
    int sz = CTX_MAP_SIZE * sizeof(ctx_t); // Size of the second-level array.
    g_ctx_manager.ctx_map[h] = lv2 = malloc(sz);
    if (!lv2) { // Check malloc failure.
      perror("malloc for ctx_map second level in ctx_add");
      // Consider more robust error handling, e.g., returning an error code.
      // For now, if allocation fails, the context won't be added to the map or
      // list.
      return;
    }
    memset(
        lv2, 0,
        sz); // Initialize the new second-level array to zeros (NULL pointers).
  }
  lv2[l] = c; // Store the pointer to the context in the map.

  // Add the context to the end of the global doubly linked list (l_prev,
  // l_next). This list allows iterating over all contexts, e.g., for cleanup or
  // periodic checks.
  if (g_ctx_manager.ctx_ltail == NULL) { // If the list is currently empty.
    g_ctx_manager.ctx_ltail = g_ctx_manager.ctx_lhead =
        c; // It's now the only element.
    c->l_prev = c->l_next = NULL;
  } else { // Add to the tail of the existing list.
    g_ctx_manager.ctx_ltail->l_next = c;
    c->l_prev = g_ctx_manager.ctx_ltail;
    c->l_next = NULL; // This context is now the new tail.
    g_ctx_manager.ctx_ltail = c;
  }
}

/**
 * @brief Removes a context from all internal management structures.
 *
 * This includes:
 * - The two-level context map (`g_ctx_manager.ctx_map`).
 * - The global doubly linked list of all contexts (`g_ctx_manager.ctx_lhead`,
 * `g_ctx_manager.ctx_ltail`).
 * - The send queue (`g_ctx_manager.ctx_qhead`, `g_ctx_manager.ctx_qtail`) if
 * the context is currently in it. After removal, the context's own list
 * pointers are cleared to prevent dangling references.
 *
 * @param c The context (ctx_t) to delete.
 */
static void ctx_del(ctx_t c) {
  uint32_t uaddr =
      ntohl(c->addr.sin_addr.s_addr); // IP address in host byte order.
  uint32_t h = uaddr >> 16;    // Upper 16 bits for the first level index.
  uint32_t l = uaddr & 0xffff; // Lower 16 bits for the second level index.
  ctx_t *lv2 = g_ctx_manager.ctx_map[h];

  // Remove from the context map.
  if (lv2) {
    lv2[l] = NULL; // Set the map entry to NULL.
  }

  // Remove from the global doubly linked list (l_prev, l_next).
  if (c->l_prev) { // If it has a previous element.
    c->l_prev->l_next = c->l_next;
  } else { // Otherwise, it was the head of the list.
    g_ctx_manager.ctx_lhead = c->l_next;
  }
  if (c->l_next) { // If it has a next element.
    c->l_next->l_prev = c->l_prev;
  } else { // Otherwise, it was the tail of the list.
    g_ctx_manager.ctx_ltail = c->l_prev;
  }

  // Remove from the send queue (q_prev, q_next) if it's part of it.
  // A context is in the send queue if its q_prev is not NULL, OR if it is the
  // queue head.
  if (c->q_prev || (c == g_ctx_manager.ctx_qhead)) {
    if (c->q_prev) { // If it's not the head but is in the queue.
      c->q_prev->q_next = c->q_next;
    } else { // It was the head of the queue.
      g_ctx_manager.ctx_qhead = c->q_next;
    }
    if (c->q_next) { // If it's not the tail.
      c->q_next->q_prev = c->q_prev;
    } else { // It was the tail of the queue.
      g_ctx_manager.ctx_qtail = c->q_prev;
    }
    // If the queue becomes empty after removal (head is now NULL)
    if (g_ctx_manager.ctx_qhead == NULL) {
      g_ctx_manager.ctx_qtail = NULL; // Ensure tail is also NULL.
    }
    g_ctx_manager.ctx_qlen--; // Decrement queue length as an item was removed.
  }

  // Clear the context's own list pointers after removal to avoid dangling
  // references from the context itself back into potentially invalid parts of
  // the lists.
  c->l_prev = c->l_next = NULL;
  c->q_prev = c->q_next = NULL;
}

/**
 * @brief Looks up a context by its IP address (provided in network byte order).
 *
 * @param addr The IP address (in_addr_t, network byte order) of the context to
 * find.
 * @return Pointer to the context (ctx_t) if found, otherwise NULL.
 */
ctx_t ctx_lookup(in_addr_t addr) {
  uint32_t uaddr = ntohl(addr); // Convert to host byte order for calculations.
  uint32_t h = uaddr >> 16;     // Upper 16 bits for first-level index.
  uint32_t l = uaddr & 0xffff;  // Lower 16 bits for second-level index.
  ctx_t *lv2 = g_ctx_manager.ctx_map[h];

  if (!lv2) { // Second level map not allocated for this /16 prefix.
    return NULL;
  }
  return lv2[l]; // Return context or NULL if not found at specific /32.
}

/**
 * @brief Enqueues a context into the send queue if it's not already queued.
 *
 * The send queue holds contexts that are ready to have an ICMP request sent.
 * A context is considered already in the queue if it's the head or has a q_prev
 * pointer. This function adds the context to the tail of the queue.
 *
 * @param c The context (ctx_t) to enqueue.
 */
void ctx_enqueue(ctx_t c) {
  // Check if the context is already effectively in the queue to prevent
  // duplicates. A context is considered in the queue if it's the head, or if
  // its q_prev is set. This also implicitly covers if it's the tail (as q_prev
  // would be set unless it's also the head). The original `if (c->q_next)` was
  // a bug, this is more robust.
  if (c->q_prev != NULL || c == g_ctx_manager.ctx_qhead) {
    return; // Already in queue or is the head.
  }

  // Add to the tail of the send queue.
  if (g_ctx_manager.ctx_qtail) {         // Queue is not empty.
    g_ctx_manager.ctx_qtail->q_next = c; // Link previous tail to new context.
    c->q_prev = g_ctx_manager.ctx_qtail; // Link new context to previous tail.
    g_ctx_manager.ctx_qtail = c;         // New context is now the tail.
    c->q_next = NULL;                    // Tail's next is NULL.
  } else {                               // Queue is empty.
    g_ctx_manager.ctx_qtail = g_ctx_manager.ctx_qhead =
        c; // New context is both head and tail.
    c->q_next = c->q_prev = NULL;
  }
  g_ctx_manager.ctx_qlen++; // Increment queue length.
}

/**
 * @brief Dequeues a context from the head of the send queue.
 *
 * Removes the context from the queue and updates head/tail pointers and length.
 * The dequeued context's queue pointers (q_prev, q_next) are cleared.
 *
 * @return Pointer to the dequeued context (ctx_t), or NULL if the queue is
 * empty.
 */
ctx_t ctx_dequeue() {
  if (g_ctx_manager.ctx_qhead) {       // If the queue is not empty.
    ctx_t c = g_ctx_manager.ctx_qhead; // Get the context at the head.

    g_ctx_manager.ctx_qhead = c->q_next; // Move head to the next context.

    if (g_ctx_manager
            .ctx_qhead) { // If the queue is still not empty after removal.
      g_ctx_manager.ctx_qhead->q_prev =
          NULL;                       // The new head has no predecessor.
    } else {                          // The queue became empty.
      g_ctx_manager.ctx_qtail = NULL; // Clear the tail pointer as well.
    }

    // Clear queue linkage of the dequeued context.
    c->q_prev = c->q_next = NULL;
    g_ctx_manager.ctx_qlen--; // Decrement queue length.
    return c;                 // Return the dequeued context.
  } else {                    // Queue is empty.
    return NULL;
  }
}

/**
 * @brief Creates a new ping context or retrieves an existing one for the given
 * target IP string.
 *
 * If a context for the target IP already exists, it's returned. Otherwise, a
 * new context is allocated, initialized (IP address, ICMP header, default
 * parameters), and added to the global context management structures.
 *
 * @param tgt A string representing the target IP address (e.g., "1.2.3.4").
 * @param io Pointer to the `io_ctx` structure containing shared socket
 * information.
 * @return Pointer to the created or retrieved context (ctx_t), or NULL if the
 *         target IP string is invalid or memory allocation fails.
 */
ctx_t ctx_new(char *tgt, struct io_ctx *io) {
  in_addr_t addr; // To store network byte order IP address.
  // Convert the target IP string to a network address structure.
  if (inet_pton(AF_INET, tgt, &addr) <= 0) {
    return NULL;
  }

  ctx_t c = ctx_lookup(addr);
  if (c != NULL) {
    return c;
  }

  c = malloc(sizeof(struct ctx));
  memset(c, 0, sizeof(struct ctx));
  c->seq = DEFAULT_CTX_SEQ;
  c->interval = DEFAULT_CTX_INTERVAL;
  c->loss_thr = DEFAULT_CTX_LOSS_THR;

  c->addr.sin_family = AF_INET; // Set address family.
  c->addr.sin_addr.s_addr =
      addr; // Set IP address (already in network byte order).
  // Store the string representation of the target IP.
  // inet_ntop(AF_INET, &addr, c->tgt, sizeof(c->tgt)); // Not strictly needed
  // if tgt is already canonical
  strncpy(c->tgt, tgt,
          sizeof(c->tgt) - 1); // Copy target string, ensuring null termination.
  c->tgt[sizeof(c->tgt) - 1] = '\0';

  // Initialize ICMP header fields.
  c->icmp_hdr.type = ICMP_ECHO; // ICMP Echo Request.
  c->icmp_hdr.un.echo.id =
      htons(getpid() & 0xFFFF); // Use lower 16 bits of PID for ID.
  // Sequence number is set per packet in ctx_make_request using c->seq, which
  // is then incremented. The c->icmp_hdr.un.echo.sequence field in the context
  // is updated before each send in ctx_make_request. No need to set it to
  // htons(1) here as it was previously.

  c->timeout.data = c; // Store pointer to context in timer data for callback.
  c->state = c->last_state = STATE_INIT; // Initialize job state.

  c->io = io; // Assign shared I/O context.

  ctx_add(c); // Add to global map and list.
  // Do not enqueue here; let the main logic (e.g. load_target_list or
  // timeout_cb) decide when to send first ping.
  return c;
}

/**
 * @brief Frees a context structure and removes it from internal management.
 *
 * @param ctx The context to free.
 */
void ctx_free(ctx_t ctx) {
  ctx_del(ctx); // Remove from map, list, and queue.
  free(ctx);    // Free the memory allocated for the context structure.
}

/**
 * @brief Calculates the difference in nanoseconds between two timespec
 * structures.
 *
 * @param t1 The start timespec.
 * @param t2 The end timespec.
 * @return The difference (t2 - t1) in nanoseconds. Returns MIN_TS_DIFF_RESULT
 * if t2 is not greater than t1.
 */
static inline uint64_t ts_diff(struct timespec t1, struct timespec t2) {
  uint64_t ts1 = t1.tv_sec * NANOSECONDS_PER_SECOND + t1.tv_nsec;
  uint64_t ts2 = t2.tv_sec * NANOSECONDS_PER_SECOND + t2.tv_nsec;
  return ts2 > ts1 ? ts2 - ts1 : MIN_TS_DIFF_RESULT;
};

static inline uint64_t ts2ms(struct timespec t) {
  return (t.tv_sec * MILLISECONDS_PER_SECOND) +
         (t.tv_nsec / NANOSECONDS_PER_MILLISECOND);
}

// Update ctx_set_state to use job_state_e
/**
 * @brief Sets the current and last state of a context.
 * @param c The context to update.
 * @param s The new state (job_state_e) to set.
 */
static void ctx_set_state(ctx_t c, job_state_e s) {
  c->last_state = c->state; // Save current state as last_state before updating.
  c->state = s;             // Set the new current state.
}

/**
 * @brief Handles a timeout event for a context.
 *
 * This function is invoked by the libev event loop when a context's timer
 * expires, indicating that an ICMP echo reply has not been received within the
 * expected time. It increments loss counters, updates the context's state based
 * on the number of consecutive losses (e.g., transitioning from UP to LOSS, or
 * LOSS to DOWN), adjusts the timer for the next ping attempt (applying backoff
 * if in DOWN or INIT state), and re-queues the context for sending.
 *
 * @param c The context (ctx_t) that has timed out.
 */
void ctx_handle_timeout(ctx_t c) {
  // This function is called when a timeout occurs for a context (target),
  // meaning an expected ICMP reply was not received in time.

  if (c->rtt_ns ==
      0) { // rtt_ns == 0 implies no reply was received for the last sent packet
    c->loss++; // Increment consecutive loss counter

    // State transition logic based on current state and loss count:
    // - STATE_INIT: Remains in STATE_INIT. Loss count increases. This state
    // persists until the first reply.
    // - STATE_UP: Transitions to STATE_LOSS on the first timeout. This
    // indicates a potential issue.
    // - STATE_LOSS: If loss count exceeds loss_thr, transitions to STATE_DOWN.
    // Otherwise, remains in STATE_LOSS.
    // - STATE_DOWN: Remains in STATE_DOWN. Loss count continues to increase.
    switch (c->state) {
    case STATE_INIT:
      // Stays in INIT until a reply is received. Loss count is tracked.
      break;
    case STATE_UP:
      // First timeout when in UP state, transition to LOSS.
      ctx_set_state(c, STATE_LOSS);
      fprintf(stderr, "LOSS:%s\n", c->tgt); // Log state change.
      break;
    case STATE_LOSS:
      // Already in LOSS state, check if loss threshold is exceeded.
      if (c->loss > c->loss_thr) {
        // Loss threshold exceeded, transition to DOWN state.
        ctx_set_state(c, STATE_DOWN);
        fprintf(stderr, "DOWN:%s\n", c->tgt); // Log state change.
      }
      // If loss_thr not exceeded, remains in STATE_LOSS.
      break;
    case STATE_DOWN:
      // Remains in DOWN state. Loss count continues to increment.
      break;
    }
  }
  // If rtt_ns is not 0, it means a reply was received for the last packet
  // before this timeout was triggered. In such cases, reply handling
  // (ctx_handle_reply) would have already updated the state. Thus, no state
  // change or loss increment is done here based on timeout.

  // Backoff timer adjustment for states INIT or DOWN.
  // This progressively increases the timeout duration if the target remains
  // unresponsive.
  if (c->state == STATE_DOWN || c->state == STATE_INIT) {
    // Calculate backoff duration:
    // Base delay is proportional to how many losses have occurred beyond the
    // threshold. If losses are not yet beyond threshold (e.g., in STATE_INIT or
    // early STATE_LOSS), a minimum factor
    // (MIN_BACKOFF_DELAY_IF_LOSS_BELOW_THRESHOLD) is used. This base delay is
    // then scaled by DEFAULT_BACKOFF_FACTOR.
    float d =
        (c->loss > c->loss_thr ? (c->loss - c->loss_thr)
                               : MIN_BACKOFF_DELAY_IF_LOSS_BELOW_THRESHOLD) *
        DEFAULT_BACKOFF_FACTOR;

    // Ensure the delay is at least MIN_EFFECTIVE_BACKOFF_DELAY (e.g. 1.0
    // second).
    if (d <= 0) // This check is mostly a safeguard.
      d = MIN_EFFECTIVE_BACKOFF_DELAY;

    // Cap the delay at MAX_BACKOFF_INTERVAL_SEC to prevent excessively long
    // waits.
    if (d > MAX_BACKOFF_INTERVAL_SEC)
      d = MAX_BACKOFF_INTERVAL_SEC;

    // Add a small random component (0.0 to 0.9 seconds) to the delay
    // to prevent synchronized thundering herd effects if many pings time out
    // simultaneously.
    d += (double)(rand() % BACKOFF_RANDOM_MODULO) / BACKOFF_RANDOM_DIVISOR;

    // Set the timer for the next ping attempt with the calculated backoff
    // delay.
    ev_timer_set(&c->timeout, d, d);
    // printf("loss=%f\n",d); // Debug print for backoff delay
  }
  // Re-enqueue the context for the next ping attempt.
  // If the state is UP or LOSS (and not yet DOWN), the timer would have been
  // reset by successful reply handling or continues with its regular interval
  // if this timeout was for a packet that arrived late (rtt_ns != 0). If state
  // is DOWN or INIT, ev_timer_set above has set the new backoff interval.
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
  sum += (sum >> 16); // Add carry again to ensure all bits are processed.
  ret = ~sum;         // One's complement of the sum is the checksum.

  return ret;
}

/**
 * @brief Prepares an ICMP Echo Request packet in the provided buffer.
 *
 * This function populates the ICMP header fields (type, ID, sequence number)
 * for the given context, calculates the ICMP checksum, and copies the complete
 * ICMP header into the `buf`. The context's internal sequence number (`c->seq`)
 * is incremented after being used for the packet. `c->rtt_ns` is reset to 0,
 * as RTT is measured from send to receive of this specific packet.
 *
 * @param c The context (ctx_t) for which to create the request.
 * @param buf Buffer where the ICMP packet will be constructed. This buffer
 * should be large enough to hold the ICMP header (and any payload, though this
 *            function only handles the header).
 * @param len The length of the data to checksum. For a standard ICMP echo
 * request without extra payload, this would be `sizeof(struct icmphdr)`.
 */
void ctx_make_request(ctx_t c, uint8_t *buf, int len) {
  c->rtt_ns = 0; // Reset Round Trip Time for this new request.

  // Set the ICMP sequence number for this packet from the context's current
  // sequence counter. htons() converts it to network byte order. The context's
  // sequence counter is then incremented for the next packet.
  c->icmp_hdr.un.echo.sequence = htons(c->seq++);

  // ICMP ID (c->icmp_hdr.un.echo.id) is assumed to be already set (e.g., during
  // ctx_new using PID).

  c->icmp_hdr.checksum =
      0; // Checksum field must be zero before calculating the checksum itself.

  // Copy the (almost complete) ICMP header into the buffer.
  // The checksum will be calculated over this data.
  memcpy(buf, &c->icmp_hdr, sizeof(c->icmp_hdr));
  c->icmp_hdr.checksum = icmp_csum((uint16_t *)buf, len);
  memcpy(buf, &c->icmp_hdr, sizeof(c->icmp_hdr));
}

void ctx_update_ts(ctx_t c, int tx, struct timespec *ts) {
  struct timespec *tgt = (tx == CTX_TS_TX) ? &c->ts_tx : &c->ts_rx;
  if (ts) { // If an external timestamp is provided (e.g., from kernel ancillary
            // data via msghdr).
    memcpy(tgt, ts, sizeof(struct timespec)); // Copy the provided timestamp.
  } else { // Otherwise, get the current system time.
    // CLOCK_REALTIME is used here, which represents the actual wall-clock time.
    // It can be affected by system time changes (e.g., NTP adjustments).
    // For measuring intervals unaffected by wall-clock changes, CLOCK_MONOTONIC
    // might be an alternative, but packet timestamps from the kernel
    // (SO_TIMESTAMPING/SO_TIMESTAMPNS) are typically based on CLOCK_REALTIME.
    clock_gettime(CLOCK_REALTIME, tgt);
  }
}

/**
 * @brief Writes a log entry for the current state or event of a context.
    uint64_t t = c->rtt_ns = ts_diff(c->ts_tx, c->ts_rx);
    log_write("%lu,%s,%lf,%d,%f,%d,%d,%u\n", ts2ms(c->ts_tx), c->tgt,
              t / 1000000.0, c->loss, c->timeout.repeat, c->last_state,
              c->state, c->ip_ttl);
  } else if (c->state != STATE_INIT) {
    // For states other than STATE_INIT and STATE_UP (i.e., STATE_LOSS,
STATE_DOWN),
    // log -1 for RTT, as there was no successful reply.
    log_write("%lu,%s,-1,%d,%f,%d,%d,%u\n", ts2ms(c->ts_tx), c->tgt, c->loss,
              c->timeout.repeat, c->last_state, c->state, c->ip_ttl);
  }
  // Note: Events in STATE_INIT are not logged here until they transition to
another state.
  // This is because typically we only log after a ping attempt (timeout or
reply).
  // A ctx in STATE_INIT might not have had a ping attempt yet if it's newly
added
  // and its initial delay timer hasn't fired.
}

/**
 * @brief Handles an incoming ICMP reply packet.
 *
 * Parses the IP and ICMP headers from the raw packet buffer, validates the
reply
 * (ensuring it's an ECHOREPLY, and that sequence number and ID match the sent
packet),
 * updates the context's state to STATE_UP, resets loss counters, and adjusts
the
 * ping timer to the regular interval.
 *
 * @param c The context (ctx_t) to which this reply belongs.
 * @param buf Buffer containing the received IP packet, starting with the IP
header,
 *            followed by the ICMP header and payload.
 * @return 0 on successful handling of a valid echo reply.
 *         -1 if the ICMP type is not ECHOREPLY.
 *         -2 if the ICMP sequence number does not match.
 *         -3 if the ICMP ID does not match.
 */
int ctx_handle_reply(ctx_t c, uint8_t *buf) {
  // IP header is at the beginning of the buffer.
  // Calculate IP header length: lower 4 bits of first byte (IHL field) * 4
  // bytes.
  uint8_t ip_len = (IP_IHL_MASK & (*buf)) * IP_HEADER_LENGTH_MULTIPLIER;
  c->ip_ttl =
      buf[IP_TTL_OFFSET]; // Extract TTL from IP header (standard offset 8).

  // ICMP header starts immediately after the IP header.
  struct icmphdr *icmp_hdr = (void *)(buf + ip_len);

  // Validate ICMP reply type: must be ECHOREPLY.
  if (icmp_hdr->type != ICMP_ECHOREPLY) {
    // fprintf(stderr, "Unexpected ICMP type: %d for %s\n", icmp_hdr->type,
    // c->tgt); // Debug
    return -1; // Not an echo reply.
  }

  // Validate ICMP sequence number.
  // c->icmp_hdr.un.echo.sequence holds the last sent sequence number, already
  // in network byte order because it was set using htons() in ctx_make_request
  // before sending.
  if (icmp_hdr->un.echo.sequence != c->icmp_hdr.un.echo.sequence) {
    // Debugging for sequence mismatch:
    // fprintf(stderr, "Seq mismatch for %s: sent %u, got %u\n",
    //         c->tgt, ntohs(c->icmp_hdr.un.echo.sequence),
    //         ntohs(icmp_hdr->un.echo.sequence));
    return -2; // Sequence number mismatch.
  }

  // Validate ICMP ID.
  // c->icmp_hdr.un.echo.id holds the ICMP ID (PID-based), already in network
  // byte order.
  if (icmp_hdr->un.echo.id != c->icmp_hdr.un.echo.id) {
    // Debugging for ID mismatch:
    // fprintf(stderr, "ID mismatch for %s: sent %u, got %u\n",
    //         c->tgt, ntohs(c->icmp_hdr.un.echo.id),
    //         ntohs(icmp_hdr->un.echo.id));
    return -3; // Identifier mismatch.
  }

  /* State transition based on receiving a valid reply:
  INIT -> UP
  U reply U
  D reply U
  L reply U
  */
  // A valid ICMP ECHOREPLY has been received.

  // Regardless of the previous state, receiving a valid reply transitions the
  // state to UP.
  ctx_set_state(c, STATE_UP);

  // Log to stderr if the target's state has changed to UP from a problem state
  // (LOSS or DOWN) or from the initial state. This highlights recovery or
  // initial contact. Note: c->state is now STATE_UP due to the call above. We
  // check c->last_state to understand the transition.
  if (c->last_state == STATE_LOSS || c->last_state == STATE_DOWN ||
      c->last_state == STATE_INIT) {
    fprintf(stderr, "UP:%s\n",
            c->tgt); // Log recovery or initial successful ping.
  }
  // If last_state was already STATE_UP, no specific log message for "still UP"
  // is printed here, but the ping success will be recorded by ctx_write_log.

  c->loss = 0; // Reset consecutive loss counter because a reply was
               // successfully received.

  // Reset the ping timer to the regular interval defined for this context
  // (c->interval), as the target has proven to be responsive. The ts_rx
  // (receive timestamp) is set by read_cb via ctx_update_ts using kernel
  // timestamps.
  ev_timer_set(&c->timeout, c->interval, c->interval);
  return 0;
}

/**
 * @brief Returns the current number of contexts in the send queue.
 *
 * This function is an accessor for the `ctx_qlen` member of the global
 * `g_ctx_manager`.
 *
 * @return The number of contexts currently awaiting transmission in the send
 * queue.
 */
int get_ctx_qlen(void) { return g_ctx_manager.ctx_qlen; }

/**
 * @brief Returns the head of the global linked list of all active contexts.
 *
 * This list (`g_ctx_manager.ctx_lhead`) can be used to iterate over all targets
 * currently being managed by the application, for purposes like reloading or
 * cleanup. This function is an accessor for the `ctx_lhead` member of the
 * global `g_ctx_manager`.
 *
 * @return Pointer to the first context (ctx_t) in the global list, or NULL if
 * the list is empty.
 */
ctx_t get_ctx_lhead(void) { return g_ctx_manager.ctx_lhead; }
