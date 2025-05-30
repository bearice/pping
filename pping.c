#include "pping.h"

#include <getopt.h>

// Named Constants for Magic Numbers
#define RX_BUFFER_SIZE 2048
#define RX_CONTROL_BUFFER_SIZE 4096
#define TX_BUFFER_SIZE 128
#define TX_CONTROL_BUFFER_SIZE                                                 \
  4096 // For ctlbuf in write_cb, though msg_controllen is 0
static const int DEFAULT_LOG_LEN = 1000 * 1000 * 10;
static const int SOCKET_FLAG_TIMESTAMPNS_VALUE = 1;
static const double STAT_TIMER_INTERVAL = 1.0;
#define TARGET_LIST_READ_BUFFER_SIZE 32
static const int DEFAULT_SOCKET_COUNT = 1;

// Define the new structures
struct pping_stats {
  int rx_cnt;
  int tx_cnt;
  int to_cnt;
  int eq_cnt;
  int wt_cnt;
  int we_cnt;
  int re_1;
  int re_2;
  int re_3;
  int re_cnt;
  int bo_cnt;
};

struct pping_config {
  float interval;
  float slowstart;
  int loss_thr;
  int reload_cnt;
  int verbose;
};

// Instantiate the structures globally
static struct pping_stats global_stats = {0};
static struct pping_config global_config = {.interval = 1.0,
                                            .slowstart = 1.0,
                                            .loss_thr = 10,
                                            .reload_cnt = 0,
                                            .verbose = 0};

struct io_ctx {
  int sock;
  ev_io r;
  ev_io w;
};

/**
 * @brief Extracts a timestamp from ancillary data in a received network
 * message.
 *
 * This function iterates through the control messages (cmsghdr) attached to a
 * msghdr to find kernel-provided timestamps. It checks for both SO_TIMESTAMPING
 * (which can provide software, hardware, or hardware-transformed timestamps)
 * and SO_TIMESTAMPNS (which provides nanosecond-precision software timestamps).
 *
 * For SO_TIMESTAMPING, the kernel provides an array of three timespec
 * structures:
 *  - ts[0]: Software timestamp.
 *  - ts[1]: (Deprecated) SCM_TSTAMP_SCHED / Hardware timestamp transformed to
 * software clock.
 *  - ts[2]: Raw hardware timestamp.
 * This function prioritizes ts[2] (raw hardware) if its tv_sec is non-zero.
 * Otherwise, it falls back to ts[0] (software).
 *
 * If SO_TIMESTAMPING is not found or does not yield a preferred timestamp,
 * it looks for SO_TIMESTAMPNS. The first suitable timestamp found is returned.
 *
 * @param msg Pointer to the msghdr structure from which to extract the
 * timestamp.
 * @return Pointer to the struct timespec containing the timestamp, or NULL if
 * no suitable timestamp is found within the control messages.
 */
static struct timespec *find_ts(struct msghdr *msg) {
  struct timespec *ts_software = NULL;
  struct timespec *ts_hardware_raw = NULL;

  // Iterate over all control messages (ancillary data) using CMSG_FIRSTHDR and
  // CMSG_NXTHDR.
  for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL;
       cmsg = CMSG_NXTHDR(msg, cmsg)) {
    // Useful for debugging:
    // printf("level=%d, type=%d, len=%zu\n", cmsg->cmsg_level, cmsg->cmsg_type,
    // cmsg->cmsg_len);

    if (cmsg->cmsg_level == SOL_SOCKET) {
      if (cmsg->cmsg_type == SO_TIMESTAMPING) {
        // Data part of cmsg is an array of struct timespec.
        struct timespec *timestamps = (struct timespec *)CMSG_DATA(cmsg);
        // ts[0] is software, ts[1] is h/w transformed (deprecated or specific
        // uses), ts[2] is raw h/w. Prioritize raw hardware timestamp if its
        // tv_sec is non-zero.
        if (timestamps[2].tv_sec != 0 || timestamps[2].tv_nsec != 0) {
          ts_hardware_raw = &timestamps[2];
          break; // Found the best possible timestamp (raw hardware), no need to
                 // look further.
        }
        // If raw hardware is not valid, consider software timestamp from this
        // cmsg.
        if (ts_software == NULL &&
            (timestamps[0].tv_sec != 0 || timestamps[0].tv_nsec != 0)) {
          ts_software = &timestamps[0];
        }
      } else if (cmsg->cmsg_type == SO_TIMESTAMPNS) {
        // SO_TIMESTAMPNS provides a single software timestamp.
        // Only consider this if we haven't found a hardware timestamp from
        // SO_TIMESTAMPING.
        if (ts_hardware_raw == NULL && ts_software == NULL) {
          struct timespec *temp_ts = (struct timespec *)CMSG_DATA(cmsg);
          if (temp_ts->tv_sec != 0 || temp_ts->tv_nsec != 0) {
            ts_software = temp_ts;
          }
        }
      }
    }
  }

  // Prefer hardware timestamp if found and valid.
  if (ts_hardware_raw) {
    // puts("using raw hardware timestamp"); // Optional debug
    return ts_hardware_raw;
  }
  // Otherwise, use software timestamp if found and valid.
  if (ts_software) {
    // puts("using software timestamp"); // Optional debug
    return ts_software;
  }

  // If no suitable timestamp is found.
  return NULL;
}

// int rx_cnt = 0, tx_cnt = 0, to_cnt = 0, eq_cnt = 0, wt_cnt = 0, we_cnt = 0,
//     re_1 = 0, re_2 = 0, re_3 = 0, re_cnt = 0, bo_cnt = 0; // Old global
//     counters, now in global_stats struct.

/**
 * @brief Callback function for handling readable events on a socket.
 *
 * This function is invoked by the event loop when an ICMP reply (or potentially
 * an error) is received on a socket. It reads the message, extracts timestamps,
 * looks up the corresponding context, updates the context with the reply data,
 * and logs the event.
 *
 * @param EV_A The libev event loop.
 * @param w The ev_io watcher that triggered this callback.
 * @param revents The type of events that occurred (should be EV_READ).
 */
static void read_cb(EV_P_ ev_io *w, int revents) {
  global_stats.rx_cnt++; // Increment total received packet counter.
  unsigned char buf[RX_BUFFER_SIZE];
  char ctlbuf[RX_CONTROL_BUFFER_SIZE];
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
    global_stats.re_cnt++;
    global_stats.re_1++;
    // fprintf(stderr, "events=%d\n", revents);
    // exit(1);
    // perror("recvfrom");
    return;
  }
  ctx_t c = ctx_lookup(addr.sin_addr.s_addr);
  //   printf("read c=%x\n", c);
  if (!c) {
    global_stats.re_2++;
    global_stats.re_cnt++;
    return;
  }
  struct timespec *ts = find_ts(&msg);
  ctx_update_ts(c, CTX_TS_RX, ts);
  ret = ctx_handle_reply(c, buf);
  if (ret == 0)
    ctx_write_log(c);
  else {
    global_stats.re_3++;
    global_stats.re_cnt++;
  }
  //   printf("repeat=%f\n", c->timeout.repeat); // Debug print for timer repeat
  //   value
  ev_timer_again(EV_A_ & c->timeout); // Restart the timer for this context,
                                      // using its current interval.
}

/**
 * @brief Callback function for handling writable events on a socket.
 *
 * This function is invoked by the event loop when a socket is ready for
 * writing. It dequeues a context (ping target) from the send queue, constructs
 * an ICMP request, sends it, and updates the transmit timestamp for the
 * context. If the send queue is empty, it stops the write watcher for this
 * socket.
 *
 * @param EV_A The libev event loop.
 * @param w The ev_io watcher that triggered this callback.
 * @param revents The type of events that occurred (should be EV_WRITE).
 */
static void write_cb(EV_P_ ev_io *w, int revents) {
  ctx_t c = ctx_dequeue(); // Get the next context to send a ping to.
  //   printf("write c=%x\n", c); // Debug print for context being processed
  if (c) {
    global_stats.tx_cnt++;
    if (c->q_next)
      global_stats.wt_cnt++;
    uint8_t buf[TX_BUFFER_SIZE];
    char ctlbuf[TX_CONTROL_BUFFER_SIZE];
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
      global_stats.we_cnt++;
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
    ctx_update_ts(
        c, CTX_TS_TX,
        NULL); // Update transmit timestamp (no kernel TX timestamp used here).
    // ctx_write_log(c); // Logging for TX is typically done on RX or timeout.
  } else {
    // If the send queue is empty, stop watching for writable events on this
    // socket to avoid busy-looping. It will be re-enabled when new contexts are
    // enqueued.
    global_stats.eq_cnt++; // Increment empty queue counter.
    ev_io_stop(EV_A_ w);
  }
}

/**
 * @brief Callback function for handling timer events for individual ping
 * targets.
 *
 * This function is invoked by the event loop when a context's timer expires.
 * This typically means a ping request has timed out. It updates the transmit
 * timestamp (as if it were sent now, for RTT calculation if a late reply
 * arrives), handles the timeout logic (state changes, loss counting), logs the
 * event if no reply was received for the last ping, and restarts the timer for
 * the context. It also ensures the write watcher is active to send the next
 * ping.
 *
 * @param EV_A The libev event loop.
 * @param w The ev_timer watcher that triggered this callback.
 * @param revents The type of events that occurred (should be EV_TIMER).
 */
static void timeout_cb(EV_P_ ev_timer *w, int revents) {
  global_stats.to_cnt++;    // Increment total timeout counter.
  ctx_t c = (ctx_t)w->data; // Retrieve context from timer's data field.
  // char st = c->state; // Debug: old state
  //   printf("timeout c=%x st=%c\n", c, st);
  ctx_update_ts(c, CTX_TS_TX, NULL);
  ctx_handle_timeout(c);
  if (c->rtt_ns == 0)
    ctx_write_log(c);
  ev_timer_again(EV_A_ & c->timeout);
  ev_io_start(EV_A_ & c->io->w); // Ensure the write watcher is active for the
                                 // next send operation.
}

// verbose flag is part of global_config: global_config.verbose
/**
 * @brief Callback function for periodic statistics reporting.
 *
 * This function is invoked by a timer at regular intervals
 * (STAT_TIMER_INTERVAL). If verbose mode is enabled (`global_config.verbose`),
 * it prints current statistics (TX/RX counts, timeouts, errors, queue length)
 * to stderr and then resets the counters.
 *
 * @param EV_A The libev event loop.
 * @param w The ev_timer watcher that triggered this callback.
 * @param revents The type of events that occurred (should be EV_TIMER).
 */
static void stat_cb(EV_P_ ev_timer *w, int revents) {
  if (global_config.verbose)
    fprintf(stderr,
            "TX=%d RX=%d TO=%d EQ=%d WT=%d WE=%d RE=%d (%d %d %d) BO=%d Q=%d\n",
            global_stats.tx_cnt, global_stats.rx_cnt - global_stats.re_cnt,
            global_stats.to_cnt, global_stats.eq_cnt, global_stats.wt_cnt,
            global_stats.we_cnt, global_stats.re_cnt, global_stats.re_1,
            global_stats.re_2, global_stats.re_3, global_stats.bo_cnt,
            get_ctx_qlen());
  // Reset stats
  memset(&global_stats, 0, sizeof(global_stats));
  ev_timer_again(EV_A_ w);
}

// static float interval = 1.0; // Now part of global_config
// static float slowstart = 1.0; // Now part of global_config
// (global_config.slowstart) static int loss_thr = 10; // Now part of
// global_config (global_config.loss_thr) static int reload_cnt = 0; // Now part
// of global_config (global_config.reload_cnt).

// Globals for target list processing, set from main before the first call to
// load_target_list. These are used by load_target_list to access command-line
// arguments and shared resources.
static int tgt_argc; /**< Count of target arguments/files from command line. */
static char **tgt_argv; /**< Array of target arguments/files. */
static struct io_ctx
    *tgt_io; /**< Pointer to the I/O context array (sockets). */
static struct ev_loop *tgt_loop; /**< Pointer to the main event loop. */

/**
 * @brief Reads a single line (target host or IP) from a given file stream.
 *
 * Strips the newline character if present.
 *
 * @param file The file stream to read from.
 * @param target_buf Buffer to store the read line.
 * @param target_buf_size Size of the target_buf.
 * @return Pointer to target_buf if a line is successfully read, otherwise NULL
 * (EOF or error).
 */
static char *read_targets_from_file(FILE *file, char *target_buf,
                                    size_t target_buf_size) {
  if (fgets(target_buf, target_buf_size, file) != NULL) {
    size_t len = strlen(target_buf);
    // Strip newline character if present
    if (len > 0 && target_buf[len - 1] == '\n') {
      target_buf[len - 1] = '\0';
    }
    return target_buf; // Return pointer to buffer if line is read
  }
  return NULL; // fgets returned NULL (EOF or error)
}

/**
 * @brief Loads or reloads the list of ping targets.
 *
 * Targets can be specified directly on the command line or in files prefixed
 * with '@'. This function parses these arguments, creates new contexts (ctx_t)
 * for new targets, and updates existing ones. It also handles removing targets
 * that are no longer in the list during a reload (triggered by SIGUSR1).
 *
 * @return The total number of active targets after loading/reloading.
 */
static int load_target_list() {
  global_config.reload_cnt++; // Increment reload counter, used to identify
                              // stale contexts.
  int host_cnt = 0;           // Counter for active hosts in this load.
  char *tgt_ptr = NULL;       // Pointer to the current target string to process
  FILE *current_file = NULL;
  char file_read_buf[TARGET_LIST_READ_BUFFER_SIZE]; // Buffer for reading
                                                    // targets from a file
  int i = 0;

  while (i < tgt_argc) {
    if (tgt_argv[i][0] == '@') { // Argument is a file
      char *filename = &tgt_argv[i][1];
      current_file = fopen(filename, "r");
      if (!current_file) {
        perror(filename);
        i++; // Move to the next argument
        continue;
      }

      // Read targets from this file
      while ((tgt_ptr = read_targets_from_file(current_file, file_read_buf,
                                               sizeof(file_read_buf))) !=
             NULL) {
        ctx_t c = ctx_new(tgt_ptr, tgt_io);
        if (NULL == c) {
          fprintf(stderr, "Invalid ip address in file %s: %s\n", filename,
                  tgt_ptr);
          continue; // Next line in file
        }
        c->interval = global_config.interval;
        c->loss_thr = global_config.loss_thr;
        if (0 == c->reload_cnt) {
          float initial_delay = (host_cnt * global_config.slowstart / 1000.0);
          ev_timer_init(&c->timeout, timeout_cb, initial_delay, c->interval);
          ev_timer_start(tgt_loop, &c->timeout);
        }
        c->reload_cnt = global_config.reload_cnt;
        host_cnt++;
      }

      fclose(current_file);
      current_file = NULL; // Reset file pointer
    } else {               // Argument is a direct target
      tgt_ptr = tgt_argv[i];
      ctx_t c = ctx_new(tgt_ptr, tgt_io);
      if (NULL == c) {
        fprintf(stderr, "Invalid ip address: %s\n", tgt_ptr);
        i++; // Move to next argument
        continue;
      }
      c->interval = global_config.interval;
      c->loss_thr = global_config.loss_thr;
      if (0 == c->reload_cnt) {
        float initial_delay = (host_cnt * global_config.slowstart / 1000.0);
        ev_timer_init(&c->timeout, timeout_cb, initial_delay, c->interval);
        ev_timer_start(tgt_loop, &c->timeout);
      }
      c->reload_cnt = global_config.reload_cnt;
      host_cnt++;
    }
    i++; // Move to the next argument
  }

  if (global_config.reload_cnt > 1) {
    int n = 0;
    ctx_t c = get_ctx_lhead();
    while (c) {
      // printf("c=%x rc=%d rcc=%d\n", c, c->reload_cnt,
      // global_config.reload_cnt);
      if (c->reload_cnt < global_config.reload_cnt) {
        ctx_t next = c->l_next;
        ev_timer_stop(tgt_loop, &c->timeout);
        fprintf(stderr, "Invalid ip address: %s\n", tgt);
        continue;
      }
      c->interval = global_config.interval;
      c->loss_thr = global_config.loss_thr;
      if (0 == c->reload_cnt) {
        float initial_delay = (host_cnt * global_config.slowstart / 1000.0);
        // printf("%s %f\n", c->tgt, initial_delay);
        ev_timer_init(&c->timeout, timeout_cb, initial_delay, c->interval);
        ev_timer_start(tgt_loop, &c->timeout);
      }
      c->reload_cnt = global_config.reload_cnt;
      host_cnt++;
    }

    if (global_config.reload_cnt > 1) {
      int n = 0;
      ctx_t c = get_ctx_lhead();
      while (c) {
        // printf("c=%x rc=%d rcc=%d\n", c, c->reload_cnt,
        // global_config.reload_cnt);
        if (c->reload_cnt < global_config.reload_cnt) {
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

  /**
   * @brief Callback function for handling SIGUSR1 signals.
   *
   * Reloads the target list when SIGUSR1 is received.
   *
   * @param EV_A The libev event loop.
   * @param w The ev_signal watcher that triggered this callback.
   * @param revents The type of events that occurred (should be EV_SIGNAL).
   */
  static void sigusr1_cb(EV_P_ ev_signal * w, int revents) {
    fprintf(stderr, "SIGUSR1 received, reloading target list.\n");
    int host_cnt = load_target_list();
    fprintf(stderr, "Loaded %d targets.\n", host_cnt);
  }

  /**
   * @brief Callback function for handling SIGINT signals (e.g., Ctrl+C).
   *
   * Breaks the main event loop to allow for graceful shutdown.
   *
   * @param EV_A The libev event loop.
   * @param w The ev_signal watcher that triggered this callback.
   * @param revents The type of events that occurred (should be EV_SIGNAL).
   */
  static void sigint_cb(EV_P_ ev_signal * w, int revents) {
    fprintf(stderr, "\nSIGINT received, shutting down.\n");
    ev_break(EV_A, EVBREAK_ALL); // Request a full break of the event loop.
  }

  /**
   * @brief Parses command-line arguments.
   *
   * Uses getopt to parse options and populate the configuration structure and
   * other parameters.
   *
   * @param argc Argument count from main.
   * @param argv Argument vector from main.
   * @param config Pointer to the pping_config structure to populate.
   * @param log_name_ptr Pointer to a char pointer to store the log file name.
   * @param log_len_ptr Pointer to an int to store the log length.
   * @return The index of the first non-option argument in argv (optind).
   */
  static int parse_arguments(int argc, char **argv, struct pping_config *config,
                             char **log_name_ptr, int *log_len_ptr) {
    int opt;
    // Initialize log_name_ptr and log_len_ptr with defaults, they will be
    // updated if options are provided.
    *log_name_ptr = NULL;
    *log_len_ptr = DEFAULT_LOG_LEN;

    while ((opt = getopt(argc, argv, "vi:o:l:s:t:")) != -1) {
      switch (opt) {
      case 'v':
        config->verbose = 1;
        break;
      case 'o':
        *log_name_ptr = optarg;
        break;
      case 'l':
        *log_len_ptr = atoi(optarg);
        break;
      case 't':
        config->loss_thr = atoi(optarg);
        break;
      case 'i':
        config->interval = atof(optarg);
        if (config->interval <= 0) {
          fprintf(stderr, "bad interval: %s\n", optarg);
          exit(EXIT_FAILURE);
        }
        break;
      case 's':
        config->slowstart = atof(optarg);
        if (config->slowstart < 0) { // Corrected check for slowstart
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
    return optind;
  }

  /**
   * @brief Sets up sockets for sending and receiving ICMP packets.
   *
   * Creates raw sockets, sets them to non-blocking, configures timestamping
   * options, and initializes libev I/O watchers for read and write events.
   *
   * @param io Pointer to the array of io_ctx structures to initialize.
   * @param sock_cnt Number of sockets to set up (typically 1).
   * @param loop The libev event loop.
   */
  static void setup_sockets(struct io_ctx * io, int sock_cnt,
                            struct ev_loop *loop) {
    int i, flags;
    for (i = 0; i < sock_cnt;
         i++) { // Loop typically runs once due to DEFAULT_SOCKET_COUNT = 1.
      io[i].sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
      if (io[i].sock < 0) {
        perror("socket()");
        exit(EXIT_FAILURE); // Corrected from return EXIT_FAILURE
      }
      flags = fcntl(io[i].sock, F_GETFL);
      fcntl(io[i].sock, F_SETFL, flags | O_NONBLOCK);

      flags = SOCKET_FLAG_TIMESTAMPNS_VALUE; // SO_TIMESTAMPNS
      setsockopt(io[i].sock, SOL_SOCKET, SO_TIMESTAMPNS, &flags, sizeof(flags));

      ev_io_init(&io[i].r, read_cb, io[i].sock, EV_READ);
      ev_io_init(&io[i].w, write_cb, io[i].sock, EV_WRITE);

      ev_io_start(loop, &io[i].r); // Start watching for read events.
      ev_io_start(loop, &io[i].w); // Start watching for write events.
    }
  }

  /**
   * @brief Sets up libev signal watchers for SIGINT and SIGUSR1.
   *
   * @param loop The libev event loop.
   */
  static void setup_signal_handlers(struct ev_loop * loop) {
    // Static watchers are used because they need to persist for the life of the
    // program as they are registered with the event loop. If they were
    // stack-allocated in this function, they would become invalid after the
    // function returns.
    static ev_signal sigint_watcher;
    ev_signal_init(&sigint_watcher, sigint_cb,
                   SIGINT); // Initialize SIGINT watcher.
    ev_signal_start(loop, &sigint_watcher);

    static ev_signal sigusr1_watcher;
    ev_signal_init(&sigusr1_watcher, sigusr1_cb, SIGUSR1);
    ev_signal_start(loop, &sigusr1_watcher);
  }

  /**
   * @brief Main function for the pping utility.
   *
   * Initializes the application, parses arguments, sets up logging, sockets,
   * signal handlers, loads initial targets, and starts the main event loop.
   *
   * @param argc Number of command-line arguments.
   * @param argv Array of command-line argument strings.
   * @return EXIT_SUCCESS on successful completion, EXIT_FAILURE on error.
   */
  int main(int argc, char **argv) {
    char *log_name_main = NULL; // To be populated by parse_arguments
    int log_len_main = 0;       // To be populated by parse_arguments
    int opt_idx;

    opt_idx = parse_arguments(argc, argv, &global_config, &log_name_main,
                              &log_len_main);

    // multi socket support is leaved here as is, but it did not improvement, so
    // it's hardcoded to 1 (DEFAULT_SOCKET_COUNT).
    int sock_cnt = DEFAULT_SOCKET_COUNT;
    struct ev_loop *loop = EV_DEFAULT; // Get the default event loop.

    srand(time(
        NULL)); // Seed random number generator, used for backoff randomization.
    log_setup(log_name_main, log_len_main); // Initialize logging.

    // Allocate memory for I/O contexts (sockets).
    struct io_ctx *io_array = malloc(sizeof(struct io_ctx) * sock_cnt);
    if (!io_array) {
      perror("malloc for io_ctx array");
      exit(EXIT_FAILURE);
    }

    // Setup sockets for sending/receiving pings.
    setup_sockets(io_array, sock_cnt, loop);

    // Set global pointers/variables for target loading functionality.
    // These are used by load_target_list and sigusr1_cb.
    tgt_argc =
        argc -
        opt_idx; // Number of remaining arguments (targets or target files).
    tgt_argv = argv + opt_idx; // Pointer to the first target argument.
    tgt_io = io_array;         // Share the I/O context array.
    tgt_loop = loop;           // Share the event loop.

    // Load initial set of targets.
    int host_cnt = load_target_list();
    fprintf(stderr, "Loaded %d targets.\n", host_cnt);

    if (host_cnt == 0) {
      fputs("No targets to ping. Exiting.\n", stderr);
      free(io_array); // Free allocated memory before exiting.
      return 0;       // EXIT_SUCCESS can be used here too.
    }

    // Initialize and start the statistics timer.
    // This timer will periodically call stat_cb to print statistics.
    ev_timer timer_stat;
    ev_timer_init(&timer_stat, stat_cb, STAT_TIMER_INTERVAL,
                  STAT_TIMER_INTERVAL);
    ev_timer_start(loop, &timer_stat);

    // Setup signal handlers for SIGINT (Ctrl+C) and SIGUSR1 (target reload).
    setup_signal_handlers(loop);

    // Start the main event loop. This will block until ev_break is called.
    fprintf(stderr, "pping started. PID: %d. Watching %d targets.\n", getpid(),
            host_cnt);
    ev_run(loop, 0);

    // Cleanup after event loop finishes.
    fprintf(stderr, "pping finished.\n");
    free(io_array); // Free the I/O context array.
    // NOTE: log_close() or similar might be needed here if log_file is not
    // stdout and needs explicit closing. Current log.c does fclose in
    // log_rotate, but not on exit. For stdout, it's not an issue.
    return 0; // EXIT_SUCCESS
  }
