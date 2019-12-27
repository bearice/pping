#include "pping.h"

char *log_name = NULL;
FILE *log_file = NULL;
int log_len = 10000000;
int log_cnt = 0;

void log_setup(char *n, int l) {
  log_name = n;
  log_len = l;
  if (log_name) {
    log_file = fopen(log_name, "w+");
    if (!log_file) {
      perror("fopen");
      exit(EXIT_FAILURE);
    }
  } else {
    log_file = stdout;
  }
}

void log_rotate() {
  log_cnt = 0;
  if (!log_name)
    return;
  fclose(log_file);
  char new_name[256];
  strcpy(new_name, log_name);
  strcat(new_name, ".1");
  rename(log_name, new_name);
  log_file = fopen(log_name, "w+");
}

void log_write(char *format_string, ...) {
  va_list args;
  va_start(args, format_string);
  vfprintf(log_file, format_string, args);
  va_end(args);
  // fflush(log_file);
  log_cnt++;
  if (log_len && log_cnt > log_len) {
    log_rotate();
  }
}
