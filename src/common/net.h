#pragma once
#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

ssize_t readn(int fd, void *buf, size_t n);
ssize_t writen(int fd, const void *buf, size_t n);

/* han edit start */
/* han edit tls */
int net_set_timeout(int fd, int seconds);

void net_init_ssl(void);
SSL_CTX* net_create_context(int is_server);
void net_configure_context(SSL_CTX *ctx, const char *cert_file, const char *key_file);

// TLS wrappers
ssize_t ssl_readn(SSL *ssl, void *buf, size_t n);
ssize_t ssl_writen(SSL *ssl, const void *buf, size_t n);
/* han edit end */


int tcp_listen(uint16_t port);
int tcp_connect(const char *host, uint16_t port);