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

struct Buffer {
    uint8_t *data;
    size_t len;
    size_t cap;
};

static void buf_init(struct Buffer *buf) {
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

// Buffer helper functions.
static bool buf_append(struct Buffer *b, const uint8_t *src, size_t n) {
    if (b->len + n > b->cap) {
        size_t new_cap = b->cap ? b->cap : 1024;
        while (new_cap < b->len + n) {
            new_cap *= 2;
        }
        uint8_t *p = realloc(b->data, new_cap);
        if (!p) {
            return false;   // out of memory; old buffer is still valid
        }
        b->data = p;
        b->cap = new_cap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return true;
}

static void buf_consume(struct Buffer *b, size_t n) {
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

static void buf_free(struct Buffer *b) {
    free(b->data);
    buf_init(b);
}

struct Conn {
    int fd;

    bool want_read;
    bool want_write;
    bool want_close;

    struct Buffer incoming;
    struct Buffer outgoing;
};

static void fd_set_nb(int fd) {
    errno = 0;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        die("fcntl(F_GETFL)");
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        die("fcntl(F_SETFL)");
    }
}

// Returns true if it parsed one full request.
static bool try_one_request(struct Conn *conn) {
    if (conn->incoming.len < 4) {
        return false;                       // need more data
    }
    uint32_t len = 0;
    memcpy(&len, conn->incoming.data, 4);
    if (len > k_max_msg) {
        msg("message too long");
        conn->want_close = true;
        return false;
    }
    if (4 + (size_t)len > conn->incoming.len) {
        return false;                       // incomplete message
    }
    const uint8_t *request = conn->incoming.data + 4;

    printf("client says: %.*s\n", (int)len, request);

    // Build the response: header + body.
    const char reply[] = "world";
    uint32_t reply_len = (uint32_t)strlen(reply);
    if (!buf_append(&conn->outgoing, (const uint8_t *)&reply_len, 4) ||
        !buf_append(&conn->outgoing, (const uint8_t *)reply, reply_len)) {
        conn->want_close = true;            // out of memory
        return false;
    }

    // Consume AFTER you're done using `request`, since it points into the buffer.
    buf_consume(&conn->incoming, 4 + (size_t)len);
    return true;
}

static void handle_read(struct Conn *conn) {
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));
    if (rv < 0 && errno == EAGAIN) {
        return;                             // not actually ready
    }
    if (rv < 0) {
        msg("read() error");
        conn->want_close = true;
        return;
    }
    if (rv == 0) {
        msg(conn->incoming.len == 0 ? "client closed" : "unexpected EOF");
        conn->want_close = true;
        return;
    }
    if (!buf_append(&conn->incoming, buf, (size_t)rv)) {
        conn->want_close = true;            // out of memory
        return;
    }

    // Handle pipelining: there may be several requests in the buffer.
    while (try_one_request(conn)) {}

    if (conn->outgoing.len > 0) {           // we have a response to send
        conn->want_read = false;
        conn->want_write = true;
    }
}

static void handle_write(struct Conn *conn) {
    assert(conn->outgoing.len > 0);
    ssize_t rv = write(conn->fd, conn->outgoing.data, conn->outgoing.len);
    if (rv < 0 && errno == EAGAIN) {
        return;                             // not actually ready
    }
    if (rv < 0) {
        msg("write() error");
        conn->want_close = true;
        return;
    }
    buf_consume(&conn->outgoing, (size_t)rv);

    if (conn->outgoing.len == 0) {          // all data written
        conn->want_read = true;
        conn->want_write = false;
    }
}

// Slots are indexed by fd, so clients[fd] is that connection's Conn.
// NULL = no connection on this fd.
#define MAX_CLIENTS 1024
static struct Conn *clients[MAX_CLIENTS];

// NEW (3a): conn_new() - create the Conn for a freshly accepted fd.
static struct Conn *conn_new(int fd) {
    struct Conn *conn = malloc(sizeof(struct Conn));
    if (!conn) {
        return NULL;
    }
    conn->fd = fd;
    conn->want_read = true;     // a new connection starts by reading a request
    conn->want_write = false;
    conn->want_close = false;
    buf_init(&conn->incoming);
    buf_init(&conn->outgoing);
    return conn;
}

// NEW (3b): conn_free() - release both buffers, then the Conn itself.
static void conn_free(struct Conn *conn) {
    buf_free(&conn->incoming);
    buf_free(&conn->outgoing);
    free(conn);
}

// NEW (5): handle_accept() - accept one connection and register it.
static void handle_accept(int serverSocket) {
    struct sockaddr_in client_addr = {0};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(serverSocket, (struct sockaddr *)&client_addr, &addrlen);
    if (connfd < 0) {
        msg("accept() error");
        return;
    }
    if (connfd >= MAX_CLIENTS) {            // no slot for this fd
        msg("too many connections");
        close(connfd);
        return;
    }
    fd_set_nb(connfd);                      // NEW (2): accepted fds are non-blocking too
    struct Conn *conn = conn_new(connfd);
    if (!conn) {
        close(connfd);
        return;
    }
    clients[connfd] = conn;
}

int main(void) {
  int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (serverSocket < 0) die("socket()");
  int val = 1;
  if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) 
    die("setsockopt()");

  // NEW (1a): bind() to 0.0.0.0:1234
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1234);
  addr.sin_addr.s_addr = htonl(0);          // wildcard address
  if (bind(serverSocket, (const struct sockaddr *)&addr, sizeof(addr)) < 0)
    die("bind()");

  // NEW (2): the listening socket must be non-blocking
  fd_set_nb(serverSocket);

  // NEW (1b): listen()
  if (listen(serverSocket, SOMAXCONN) < 0)
    die("listen()");

  while (1) {
    struct pollfd pollFds[MAX_CLIENTS + 1];

    // 1. Build the poll interest list.
    int numberOfPollFds = 0;
    pollFds[numberOfPollFds++] = (struct pollfd) {
        .fd = serverSocket,
        .events = POLLIN
    };
    for (int fd = 0; fd < MAX_CLIENTS; fd++) {
        struct Conn *c = clients[fd];
        if (c == NULL) {
            continue;
        }
        pollFds[numberOfPollFds].fd = c->fd;
        pollFds[numberOfPollFds].events = POLLERR;
        if (c->want_read) {
            pollFds[numberOfPollFds].events |= POLLIN;
        }
        if (c->want_write) {
            pollFds[numberOfPollFds].events |= POLLOUT;
        }
        numberOfPollFds++;
    }

    // 2. Wait for readiness
    // NEW (4): poll() - block until something is ready
    int rv = poll(pollFds, (nfds_t)numberOfPollFds, -1);
    if (rv < 0 && errno == EINTR) {
        continue;                           // interrupted by a signal, retry
    }
    if (rv < 0) {
        die("poll()");
    }

    // 3. Accept new connections
    // NEW (5): the listening socket is pollFds[0]
    if (pollFds[0].revents & POLLIN) {
        handle_accept(serverSocket);
    }

    // 4. + 5. Process connection sockets (pollFds[1..])
    for (int i = 1; i < numberOfPollFds; i++) {
        uint32_t ready = pollFds[i].revents;
        if (ready == 0) {
            continue;
        }
        // NEW (6a): map the pollfd back to its Conn via the fd
        struct Conn *c = clients[pollFds[i].fd];

        // 4. Process readable clients
        if (ready & POLLIN) {
            handle_read(c);
        }
        // 5. Process writable clients
        if (ready & POLLOUT) {
            handle_write(c);
        }

        // NEW (6b): close on socket error or when the app asked to close
        if ((ready & POLLERR) || c->want_close) {
            clients[c->fd] = NULL;
            close(c->fd);
            conn_free(c);
        }
    }
  }
}
