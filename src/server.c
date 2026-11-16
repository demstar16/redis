#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
#include "util.h"

// ---------------------------------------------------------------------------
// Buffer: a growable array of bytes (C's version of std::vector<uint8_t>).
//
// It is one block of heap memory plus two numbers describing it:
//
//   bytes
//     |
//     v
//     [ H  e  l  l  o  _  _  _  _  _  _  _ ]
//       |<--- used = 5 --->|
//       |<-------------- capacity = 12 ---->|
//
//   bytes:    pointer to the first byte (NULL until the first append)
//   used:     how many bytes hold real data
//   capacity: how many bytes we have allocated (used <= capacity always)
//
// The next free byte is at (bytes + used): that is pointer arithmetic,
// "start of the block, moved forward by `used` bytes".
// ---------------------------------------------------------------------------
struct Buffer {
    uint8_t *bytes;
    size_t used;
    size_t capacity;
};

static void buffer_init(struct Buffer *buffer) {
    buffer->bytes = NULL;       // `->` means "the field of the struct this points to"
    buffer->used = 0;
    buffer->capacity = 0;
}

// Add `count` bytes from `new_bytes` to the END of the buffer.
// If they don't fit, grow the allocation first. Returns false if out of memory.
static bool buffer_append(struct Buffer *buffer, const uint8_t *new_bytes, size_t count) {
    size_t needed = buffer->used + count;
    if (needed > buffer->capacity) {
        // Start at 1024, then keep doubling until it is big enough.
        size_t new_capacity = buffer->capacity ? buffer->capacity : 1024;
        while (new_capacity < needed) {
            new_capacity *= 2;
        }
        // realloc may move the data to a new block and returns the new address.
        // Keep the result in a separate variable so a failure doesn't lose the old block.
        uint8_t *grown_bytes = realloc(buffer->bytes, new_capacity);
        if (!grown_bytes) {
            return false;       // out of memory; the old buffer is still valid
        }
        buffer->bytes = grown_bytes;
        buffer->capacity = new_capacity;
    }
    // Copy `count` bytes to the first free byte, then record that they are used.
    memcpy(buffer->bytes + buffer->used, new_bytes, count);
    buffer->used += count;
    return true;
}

// Remove the first `count` bytes by sliding everything after them to the front.
//
//   before:  [ A  B  C  D  E  _  _ ]   used = 5
//   remove 2
//   after:   [ C  D  E  _  _  _  _ ]   used = 3
//
// memmove (not memcpy) because the source and destination overlap.
static void buffer_remove_front(struct Buffer *buffer, size_t count) {
    size_t remaining = buffer->used - count;
    // First argument is the destination, second is the source, third is how many bytes to move.
    // Think of the second argument as the start of the new data.
    memmove(buffer->bytes, buffer->bytes + count, remaining);
    buffer->used = remaining;
}

// Release the memory and reset to the empty state.
static void buffer_free(struct Buffer *buffer) {
    free(buffer->bytes);
    buffer_init(buffer);
}

// ---------------------------------------------------------------------------
// Conn: everything the server remembers about one connected client.
// ---------------------------------------------------------------------------
struct Conn {
    int socket_fd;

    // What the event loop should wait for on this socket next.
    bool want_read;             // waiting for the client to send a request
    bool want_write;            // have a response waiting to be sent
    bool want_close;            // something went wrong or the client left

    struct Buffer incoming;     // bytes received but not yet parsed into requests
    struct Buffer outgoing;     // response bytes not yet sent
};

// Make a socket non-blocking: read/write/accept return immediately
// (with errno == EAGAIN) instead of waiting.
static void set_nonblocking(int fd) {
    errno = 0;

    int current_flags = fcntl(fd, F_GETFL, 0);
    if (current_flags < 0) {
        die("fcntl(F_GETFL)");
    }

    if (fcntl(fd, F_SETFL, current_flags | O_NONBLOCK) < 0) {
        die("fcntl(F_SETFL)");
    }
}

// Message format:   | 4-byte length N | N bytes of body |
// Try to handle ONE complete request at the front of conn->incoming.
// Returns true if one was handled, false if we need more data (or hit an error).
static bool try_handle_one_request(struct Conn *conn) {
    // Do we have the whole 4-byte length header yet?
    if (conn->incoming.used < k_header_size) {
        return false;                       // need more data
    }
    uint32_t body_length = 0;
    memcpy(&body_length, conn->incoming.bytes, k_header_size);
    if (body_length > k_max_msg) {
        msg("message too long");
        conn->want_close = true;
        return false;
    }

    // Have we received the whole body too?
    size_t total_length = k_header_size + (size_t)body_length;
    if (total_length > conn->incoming.used) {
        return false;                       // incomplete message, wait for more
    }
    const uint8_t *request_body = conn->incoming.bytes + k_header_size;

    printf("client says: %.*s\n", (int)body_length, request_body);

    // Build the response in the outgoing buffer: header, then body.
    const char reply_body[] = "world";
    uint32_t reply_length = (uint32_t)strlen(reply_body);
    if (!buffer_append(&conn->outgoing, (const uint8_t *)&reply_length, k_header_size) ||
        !buffer_append(&conn->outgoing, (const uint8_t *)reply_body, reply_length)) {
        conn->want_close = true;            // out of memory
        return false;
    }

    // Remove the request from the front. Do this LAST: request_body points
    // into the buffer, so it is invalid after the bytes are shifted.
    buffer_remove_front(&conn->incoming, total_length);
    return true;
}

// The socket is readable: read what's there and handle any complete requests.
static void handle_read(struct Conn *conn) {
    uint8_t read_chunk[64 * 1024];          // temporary space for one read()
    ssize_t bytes_read = read(conn->socket_fd, read_chunk, sizeof(read_chunk));
    if (bytes_read < 0 && errno == EAGAIN) {
        return;                             // not actually ready
    }
    if (bytes_read < 0) {
        msg("read() error");
        conn->want_close = true;
        return;
    }
    if (bytes_read == 0) {
        msg(conn->incoming.used == 0 ? "client closed" : "unexpected EOF");
        conn->want_close = true;
        return;
    }
    if (!buffer_append(&conn->incoming, read_chunk, (size_t)bytes_read)) {
        conn->want_close = true;            // out of memory
        return;
    }

    // Pipelining: the client may have sent several requests at once.
    while (try_handle_one_request(conn)) {}

    if (conn->outgoing.used > 0) {          // we have a response to send
        conn->want_read = false;
        conn->want_write = true;
    }
}

// The socket is writable: send as much of the response as the socket accepts.
static void handle_write(struct Conn *conn) {
    assert(conn->outgoing.used > 0);
    ssize_t bytes_written = write(conn->socket_fd, conn->outgoing.bytes, conn->outgoing.used);
    if (bytes_written < 0 && errno == EAGAIN) {
        return;                             // not actually ready
    }
    if (bytes_written < 0) {
        msg("write() error");
        conn->want_close = true;
        return;
    }
    // write() may send only part of the data; drop just the part that was sent.
    buffer_remove_front(&conn->outgoing, (size_t)bytes_written);

    if (conn->outgoing.used == 0) {         // everything sent, back to reading
        conn->want_read = true;
        conn->want_write = false;
    }
}

// Slots are indexed by socket fd, so connections_by_fd[fd] is that
// connection's Conn. NULL = no connection on this fd.
#define MAX_CONNECTIONS 1024
static struct Conn *connections_by_fd[MAX_CONNECTIONS];

// NEW (3a): conn_new() - create the Conn for a freshly accepted fd.
static struct Conn *conn_new(int socket_fd) {
    struct Conn *conn = malloc(sizeof(struct Conn));
    if (!conn) {
        return NULL;
    }
    conn->socket_fd = socket_fd;
    conn->want_read = true;     // a new connection starts by reading a request
    conn->want_write = false;
    conn->want_close = false;
    buffer_init(&conn->incoming);
    buffer_init(&conn->outgoing);
    return conn;
}

// NEW (3b): conn_free() - release both buffers, then the Conn itself.
static void conn_free(struct Conn *conn) {
    buffer_free(&conn->incoming);
    buffer_free(&conn->outgoing);
    free(conn);
}

// NEW (5): handle_accept() - accept one connection and register it.
static void handle_accept(int listen_socket) {
    struct sockaddr_in peer_address = {0};
    socklen_t address_length = sizeof(peer_address);
    int connection_fd = accept(listen_socket, (struct sockaddr *)&peer_address, &address_length);
    if (connection_fd < 0) {
        msg("accept() error");
        return;
    }
    if (connection_fd >= MAX_CONNECTIONS) { // no slot for this fd
        msg("too many connections");
        close(connection_fd);
        return;
    }
    set_nonblocking(connection_fd);         // NEW (2): accepted fds are non-blocking too
    struct Conn *conn = conn_new(connection_fd);
    if (!conn) {
        close(connection_fd);
        return;
    }
    connections_by_fd[connection_fd] = conn;
}

int main(void) {
  int listen_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_socket < 0) die("socket()");
  int enable = 1;
  if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
    die("setsockopt()");

  // NEW (1a): bind() to 0.0.0.0:1234
  struct sockaddr_in listen_address = {0};
  listen_address.sin_family = AF_INET;
  listen_address.sin_port = htons(1234);
  listen_address.sin_addr.s_addr = htonl(0);          // wildcard address
  if (bind(listen_socket, (const struct sockaddr *)&listen_address, sizeof(listen_address)) < 0)
    die("bind()");

  // NEW (2): the listening socket must be non-blocking
  set_nonblocking(listen_socket);

  // NEW (1b): listen()
  if (listen(listen_socket, SOMAXCONN) < 0)
    die("listen()");

  while (1) {
    struct pollfd poll_fds[MAX_CONNECTIONS + 1];

    // 1. Build the poll interest list.
    int poll_count = 0;
    poll_fds[poll_count++] = (struct pollfd) {
        .fd = listen_socket,
        .events = POLLIN
    };
    for (int socket_fd = 0; socket_fd < MAX_CONNECTIONS; socket_fd++) {
        struct Conn *conn = connections_by_fd[socket_fd];
        if (conn == NULL) {
            continue;
        }
        poll_fds[poll_count].fd = conn->socket_fd;
        poll_fds[poll_count].events = POLLERR;
        if (conn->want_read) {
            poll_fds[poll_count].events |= POLLIN;
        }
        if (conn->want_write) {
            poll_fds[poll_count].events |= POLLOUT;
        }
        poll_count++;
    }

    // 2. Wait for readiness
    // NEW (4): poll() - block until something is ready
    int ready_count = poll(poll_fds, (nfds_t)poll_count, -1);
    if (ready_count < 0 && errno == EINTR) {
        continue;                           // interrupted by a signal, retry
    }
    if (ready_count < 0) {
        die("poll()");
    }

    // 3. Accept new connections
    // NEW (5): the listening socket is poll_fds[0]
    if (poll_fds[0].revents & POLLIN) {
        handle_accept(listen_socket);
    }

    // 4. + 5. Process connection sockets (poll_fds[1..])
    for (int i = 1; i < poll_count; i++) {
        uint32_t ready_events = poll_fds[i].revents;
        if (ready_events == 0) {
            continue;
        }
        // NEW (6a): map the pollfd back to its Conn via the fd
        struct Conn *conn = connections_by_fd[poll_fds[i].fd];

        // 4. Process readable clients
        if (ready_events & POLLIN) {
            handle_read(conn);
        }
        // 5. Process writable clients
        if (ready_events & POLLOUT) {
            handle_write(conn);
        }

        // NEW (6b): close on socket error or when the app asked to close
        if ((ready_events & POLLERR) || conn->want_close) {
            connections_by_fd[conn->socket_fd] = NULL;
            close(conn->socket_fd);
            conn_free(conn);
        }
    }
  }
}
