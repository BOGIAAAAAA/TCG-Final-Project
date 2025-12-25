#define _POSIX_C_SOURCE 200112L

#include "net.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <netdb.h>

void conn_init(connection_t *c, int fd, SSL *ssl) {
    if (c) {
        c->fd = fd;
        c->ssl = ssl;
        c->ctx = NULL; 
    }
}

void conn_close(connection_t *c) {
    if (!c) return;
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

ssize_t conn_readn(connection_t *c, void *buf, size_t n) {
    if (!c) return -1;
    if (c->ssl) {
        size_t left = n;
        char *p = (char*)buf;
        while (left > 0) {
            int r = SSL_read(c->ssl, p, (int)left);
            if (r <= 0) {
                 int err = SSL_get_error(c->ssl, r);
                 if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue; // simplistic blocking handling
                 return -1;
            }
            left -= (size_t)r;
            p += r;
        }
        return (ssize_t)n;
    } else {
        return readn(c->fd, buf, n);
    }
}

ssize_t conn_writen(connection_t *c, const void *buf, size_t n) {
    if (!c) return -1;
    if (c->ssl) {
        size_t left = n;
        const char *p = (const char*)buf;
        while (left > 0) {
            int w = SSL_write(c->ssl, p, (int)left);
            if (w <= 0) {
                 int err = SSL_get_error(c->ssl, w);
                 if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
                 return -1;
            }
            left -= (size_t)w;
            p += w;
        }
        return (ssize_t)n;
    } else {
        return writen(c->fd, buf, n);
    }
}

/* --- Legacy (Plain) --- */

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

/* --- SSL Helpers --- */

void ssl_msg_init(void) {
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
}

SSL_CTX* ssl_init_client_ctx(void) {
    const SSL_METHOD *method = TLS_client_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        ERR_print_errors_fp(stderr);
    }
    return ctx;
}

SSL_CTX* ssl_init_server_ctx(const char *cert_path, const char *key_path) {
    const SSL_METHOD *method = TLS_server_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (!SSL_CTX_check_private_key(ctx)) {
        fprintf(stderr, "Private key does not match the certificate public key\n");
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}