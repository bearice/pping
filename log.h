/**
 * @file log.h
 * @brief Interface for the logging module of pping.
 *
 * This module provides basic logging functionality, including setting up a log
 * file (or defaulting to stdout), writing formatted log messages, and log
 * rotation based on the number of entries.
 */

/**
 * @brief Initializes the logging system.
 *
 * Sets the log output destination and rotation parameters. If `n` (log file
 * name) is NULL, logging will be directed to stdout. If `l` (log length) is 0,
 * log rotation based on length is disabled.
 *
 * @param n Name of the log file. If NULL, logs to stdout.
 * @param l Maximum number of log entries before rotation. If 0, rotation is
 * effectively disabled.
 */
void log_setup(char *n, int l);

/**
 * @brief Rotates the current log file.
 *
 * If logging to a file (not stdout), this function closes the current log file,
 * renames it (e.g., "pping.log" to "pping.log.1"), and opens a new "pping.log"
 * for subsequent log messages. This is typically triggered when the log length
 * limit is reached.
 * Does nothing if logging to stdout or if the log file name is not set.
 */
void log_rotate(void);

/**
 * @brief Writes a formatted message to the configured log destination.
 *
 * This function supports printf-style formatting. It also handles log rotation
 * by checking the number of log entries against the configured maximum length.
 *
 * @param format_string The format string (printf-style).
 * @param ... Variable arguments corresponding to the format string.
 */
void log_write(char *format_string, ...);
