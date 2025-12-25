/* han edit tls */
#define _POSIX_C_SOURCE 200112L
#include <openssl/ssl.h>
#include <openssl/err.h>


#include "net.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>

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

/* han edit start */
int net_set_timeout(int fd, int seconds) {
    if (fd < 0 || seconds < 0) return -1;

    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) return -1;

    return 0;
}
/* han edit end */

/* han edit tls */
void net_init_ssl(void) {
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
}

SSL_CTX* net_create_context(int is_server) {
    const SSL_METHOD *method;
    method = is_server ? TLS_server_method() : TLS_client_method();

    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        perror("Unable to create SSL context");
        ERR_print_errors_fp(stderr);
        return NULL;
    }
    return ctx;
}

void net_configure_context(SSL_CTX *ctx, const char *cert_file, const char *key_file) {
    if (SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) <= 0 ) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
}

ssize_t ssl_readn(SSL *ssl, void *buf, size_t n) {
    size_t left = n;
    char *p = (char*)buf;
    while (left > 0) {
        int r = SSL_read(ssl, p, (int)left);
        if (r <= 0) {
            int err = SSL_get_error(ssl, r);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue; // simplistic blockage handling
            // For MVP timeout, if we get SYSCALL error and errno is EAGAIN, it might be timeout, but SSL_read handles it slightly differently.
            // Simplified: return r if <= 0
            return (ssize_t)r; // 0 or -1
        }
        left -= (size_t)r;
        p += r;
    }
    return (ssize_t)n;
}

ssize_t ssl_writen(SSL *ssl, const void *buf, size_t n) {
    size_t left = n;
    const char *p = (const char*)buf;
    while (left > 0) {
        int w = SSL_write(ssl, p, (int)left);
        if (w <= 0) {
             int err = SSL_get_error(ssl, w);
             if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
             return -1;
        }
        left -= (size_t)w;
        p += w;
    }
    return (ssize_t)n;
}
/* han edit tls end */


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