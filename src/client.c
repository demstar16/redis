#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "util.h"

static int32_t query(int fd, const char *text) {
    uint32_t len = (uint32_t)strlen(text);
    if (len > k_max_msg) {
        return -1;
    }
    // send request
    char writeBuffer[4 + k_max_msg];
    memcpy(writeBuffer, &len, 4);  // assume little endian
    memcpy(&writeBuffer[4], text, len);
    int32_t err = write_all(fd, writeBuffer, 4 + len);
    if (err) {
        return err;
    }
    // 4 bytes header
    char readBuffer[4 + k_max_msg];
    errno = 0;
    err = read_full(fd, readBuffer, 4);
    if (err) {
        msg(errno == 0 ? "EOF" : "read() error");
        return err;
    }
    memcpy(&len, readBuffer, 4);  // assume little endian
    if (len > k_max_msg) {
        msg("too long");
        return -1;
    }
    // reply body
    err = read_full(fd, &readBuffer[4], len);
    if (err) {
        msg("read() error");
        return err;
    }
    // do something
    printf("server says: %.*s\n", len, &readBuffer[4]);
    return 0;
}

int main(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) die("socket()");

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1234);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
  
  int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
  if (rv < 0) die("connect");

  int32_t err = query(fd, "hello1");
  if (err) {
      goto L_DONE;
  }
  err = query(fd, "hello2");
  if (err) {
      goto L_DONE;
  }
  L_DONE:
  close(fd);
  return 0;
}
