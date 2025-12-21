#pragma once
#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>
#include <unistd.h>

ssize_t readn(int fd, void *buf, size_t n);
ssize_t writen(int fd, const void *buf, size_t n);

int tcp_listen(uint16_t port);
int tcp_connect(const char *host, uint16_t port);