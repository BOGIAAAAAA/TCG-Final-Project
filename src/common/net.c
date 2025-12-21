#define _POSIX_C_SOURCE 200112L

#include "net.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <netdb.h>

ssize_t readn(int fd, void *buf, size_t n) {
    size_t left = n;
    char *p = (char*)buf;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r == 0) return (ssize_t)(n - left);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        left -= (size_t)r;
        p += r;
    }
    return (ssize_t)n;
}

ssize_t writen(int fd, const void *buf, size_t n) {
    size_t left = n;
    const char *p = (const char*)buf;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        left -= (size_t)w;
        p += w;
    }
    return (ssize_t)n;
}

int tcp_listen(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 1024) < 0) { close(fd); return -1; }
    return fd;
}

int tcp_connect(const char *host, uint16_t port) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}