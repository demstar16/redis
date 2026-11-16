#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "util.h"

// Print a message to stderr.
void msg(const char *message) {
  fprintf(stderr, "%s\n", message);
}

// Print the message along with the current errno, then crash.
void die(const char *message) {
  int saved_errno = errno;      // save it: fprintf could overwrite errno
  fprintf(stderr, "[%d] %s\n", saved_errno, message);
  abort();
}

// Read exactly `byte_count` bytes from `fd` into `buffer`.
// read() may return fewer bytes than requested, so we loop until we have them all.
// Returns 0 on success, -1 on error or if the connection closed early.
int32_t read_full(int fd, char *buffer, size_t byte_count) {
  size_t bytes_left = byte_count;
  char *write_position = buffer;            // where the next bytes will land
  while (bytes_left > 0) {
    ssize_t bytes_read = read(fd, write_position, bytes_left);
    if (bytes_read < 0) {
      if (errno == EINTR) continue;         // interrupted by a signal, retry
      return -1;
    }
    if (bytes_read == 0) return -1;         // EOF: other side closed
    assert((size_t)bytes_read <= bytes_left);
    bytes_left -= (size_t)bytes_read;
    write_position += bytes_read;           // move forward past what we just read
  }
  return 0;
}

// Write exactly `byte_count` bytes from `buffer` to `fd`.
// write() may accept fewer bytes than offered, so we loop until all are sent.
// Returns 0 on success, -1 on error.
int32_t write_all(int fd, const char *buffer, size_t byte_count) {
  size_t bytes_left = byte_count;
  const char *read_position = buffer;       // where the next bytes come from
  while (bytes_left > 0) {
    ssize_t bytes_written = write(fd, read_position, bytes_left);
    if (bytes_written < 0) {
      if (errno == EINTR) continue;         // interrupted by a signal, retry
      return -1;
    }
    if (bytes_written == 0) return -1;
    assert((size_t)bytes_written <= bytes_left);
    bytes_left -= (size_t)bytes_written;
    read_position += bytes_written;         // move forward past what we just sent
  }
  return 0;
}
