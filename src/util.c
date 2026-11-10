#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "util.h"

void msg(const char *m) {
  fprintf(stderr, "%s\n", m);
}

void die(const char *m) {
  int err = errno;
  fprintf(stderr, "[%d] %s\n", err, m);
  abort();
}

int32_t read_full(int fd, char *buf, size_t n) {
  while (n > 0) {
    ssize_t rv = read(fd, buf, n);
    if (rv < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (rv == 0) return -1;
    assert((size_t)rv <= n);
    n -= (size_t)rv;
    buf += rv;
  }
  return 0;
}

int32_t write_all(int fd, const char *buf, size_t n) {
  while (n > 0) {
    ssize_t rv = write(fd, buf, n);
    if (rv < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (rv == 0) return -1;
    assert((size_t)rv <= n);
    n -= (size_t)rv;
    buf += rv;
  }
  return 0;
}