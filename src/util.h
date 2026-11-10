#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

enum { k_max_msg = 4096 };

void    msg(const char *m);
void    die(const char *m);
int32_t read_full(int fd, char *buf, size_t n);
int32_t write_all(int fd, const char *buf, size_t n);

#endif