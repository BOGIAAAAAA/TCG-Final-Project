#pragma once
#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

typedef struct {
    int fd;
    SSL *ssl;      // if NULL, use plain read/write
    SSL_CTX *ctx;  // context ref (optional to store here, but good for cleanup)
} connection_t;

// Connection Helpers
void conn_init(connection_t *c, int fd, SSL *ssl);
void conn_close(connection_t *c);

// Wrapper for read/write
ssize_t conn_readn(connection_t *c, void *buf, size_t n);
ssize_t conn_writen(connection_t *c, const void *buf, size_t n);

// Legacy helpers (still used potentially)
ssize_t readn(int fd, void *buf, size_t n);
ssize_t writen(int fd, const void *buf, size_t n);

int tcp_listen(uint16_t port);
int tcp_connect(const char *host, uint16_t port);

// SSL Init Helpers
void ssl_msg_init(void); // Init lib
SSL_CTX* ssl_init_server_ctx(const char *cert_path, const char *key_path);
SSL_CTX* ssl_init_client_ctx(void);