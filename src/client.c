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

// Send one request and print the server's reply.
// Message format: | 4-byte length | body of that many bytes |
static int32_t send_request(int socket_fd, const char *text) {
    uint32_t body_length = (uint32_t)strlen(text);
    if (body_length > k_max_msg) {
        return -1;
    }

    // Build the request: header first, then the body right after it.
    char send_buffer[k_header_size + k_max_msg];
    memcpy(send_buffer, &body_length, k_header_size);        // assume little endian
    memcpy(&send_buffer[k_header_size], text, body_length);
    int32_t error = write_all(socket_fd, send_buffer, k_header_size + body_length);
    if (error) {
        return error;
    }

    // Read the reply: header first, to learn how long the body is...
    char receive_buffer[k_header_size + k_max_msg];
    errno = 0;
    error = read_full(socket_fd, receive_buffer, k_header_size);
    if (error) {
        msg(errno == 0 ? "EOF" : "read() error");
        return error;
    }
    uint32_t reply_length = 0;
    memcpy(&reply_length, receive_buffer, k_header_size);    // assume little endian
    if (reply_length > k_max_msg) {
        msg("too long");
        return -1;
    }

    // ...then the body.
    error = read_full(socket_fd, &receive_buffer[k_header_size], reply_length);
    if (error) {
        msg("read() error");
        return error;
    }
    printf("server says: %.*s\n", (int)reply_length, &receive_buffer[k_header_size]);
    return 0;
}

int main(void) {
  int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) die("socket()");

  struct sockaddr_in server_address = {0};
  server_address.sin_family = AF_INET;
  server_address.sin_port = htons(1234);
  server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1

  int connect_result = connect(socket_fd, (const struct sockaddr *)&server_address,
                               sizeof(server_address));
  if (connect_result < 0) die("connect");

  int32_t error = send_request(socket_fd, "hello1");
  if (error) {
      goto L_DONE;
  }
  error = send_request(socket_fd, "hello2");
  if (error) {
      goto L_DONE;
  }
  L_DONE:
  close(socket_fd);
  return 0;
}
