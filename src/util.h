#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

enum {
    k_max_msg = 4096,       // largest message body we accept, in bytes
    k_header_size = 4       // every message starts with a 4-byte length
};

void    msg(const char *message);
void    die(const char *message);
int32_t read_full(int fd, char *buffer, size_t byte_count);
int32_t write_all(int fd, const char *buffer, size_t byte_count);

#endif
