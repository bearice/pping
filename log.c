#include "pping.h"

// Define the log_manager structure
struct log_manager {
  char *log_name;
  FILE *log_file;
  int log_len;
  int log_cnt;
};

// Instantiate the log_manager globally and statically
static struct log_manager g_log_manager = {
    .log_name = NULL,
    .log_file = NULL,
    .log_len = 10000000, // Default log length before rotation. Actual value set
                         // by DEFAULT_LOG_LEN via parse_arguments in pping.c.
    .log_cnt = 0};       // Current log entry count.

/**
 * @brief Sets up the logging mechanism.
 *
 * Initializes the log file name and maximum log length (number of entries
 * before rotation). If a log file name is provided, it opens (or creates) the
 * file for writing. If no name is provided, logs are directed to stdout.
 *
 * @param n Name of the log file. If NULL, logs to stdout.
 * @param l Maximum number of log entries before rotation. If 0, rotation is
 * effectively disabled.
 */
void log_setup(char *n, int l) {
  g_log_manager.log_name = n;
  g_log_manager.log_len = l;
  if (g_log_manager.log_name) {
    g_log_manager.log_file = fopen(g_log_manager.log_name, "w+");
    if (!g_log_manager.log_file) {
      perror("fopen");
      exit(EXIT_FAILURE);
    }
  } else {
    g_log_manager.log_file = stdout;
  }
}

/**
 * @brief Rotates the log file if logging to a file and not stdout.
 *
 * Closes the current log file, renames it by appending ".1" (e.g., "pping.log"
 * becomes "pping.log.1"), and opens a new log file with the original name.
 * Resets the log entry counter (`g_log_manager.log_cnt`).
 * This function does nothing if logging is configured to stdout (i.e.,
 * `g_log_manager.log_name` is NULL). Includes basic error handling for file
 * operations.
 */
void log_rotate() {
  g_log_manager.log_cnt = 0; // Reset log counter.

  // Do not rotate if logging to stdout (log_name is NULL) or if log_file is not
  // set (e.g., setup failed or stdout).
  if (!g_log_manager.log_name || !g_log_manager.log_file ||
      g_log_manager.log_file == stdout) {
    return;
  }

  fclose(g_log_manager.log_file); // Close the current log file.

  char new_name[256]; // Buffer for the rotated log file name.
  // Ensure buffer is large enough: strlen(log_name) + strlen(".1") + 1 for null
  // terminator.
  if (strlen(g_log_manager.log_name) + strlen(".1") >= sizeof(new_name)) {
    fprintf(stderr, "Log name too long for rotation buffer: %s\n",
            g_log_manager.log_name);
    // Attempt to reopen the original log file to continue logging without
    // rotation.
    g_log_manager.log_file = fopen(g_log_manager.log_name, "w+");
    if (!g_log_manager.log_file) {
      perror("log_rotate: fopen after failing to form rotated name");
      // Critical error, logging may stop. Consider exiting or disabling
      // logging.
    }
    return;
  }
  strcpy(new_name, g_log_manager.log_name);
  strcat(new_name, ".1"); // Append ".1" to the original log name.

  // Rename the current log file to the new (rotated) name.
  if (rename(g_log_manager.log_name, new_name) != 0) {
    perror("log_rotate: rename log file for rotation");
    // If rename fails, attempt to reopen the original log file to continue
    // logging. This might overwrite existing logs if w+ truncates.
  }

  // Open a new log file with the original name.
  g_log_manager.log_file = fopen(g_log_manager.log_name, "w+");
  if (!g_log_manager.log_file) {
    perror("log_rotate: fopen new log file after rotation");
    // Critical error if the new log file cannot be opened.
    // Logging effectively stops. Program might need to exit or disable logging.
    // For simplicity here, we don't exit, but errors will occur on subsequent
    // log_write.
  }
}

/**
 * @brief Writes a formatted string to the log file.
 *
 * Uses varargs (va_list) to handle a variable number of arguments, similar to
 * printf. Increments the log entry counter and triggers log rotation if the
 * maximum log length (`g_log_manager.log_len`) is reached and rotation is
 * enabled (log_len > 0).
 *
 * @param format_string The format string for the log message (printf-style).
 * @param ... Variable arguments corresponding to the format string.
 */
void log_write(char *format_string, ...) {
  if (!g_log_manager.log_file)
    return; // Do nothing if log file is not (or no longer) open.

  va_list args;
  va_start(args, format_string);
  vfprintf(g_log_manager.log_file, format_string, args);
  va_end(args);
  // fflush(g_log_manager.log_file);
  g_log_manager.log_cnt++;
  if (g_log_manager.log_len && g_log_manager.log_cnt > g_log_manager.log_len) {
    log_rotate();
  }
}
