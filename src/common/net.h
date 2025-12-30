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

int net_set_timeout(int fd, int seconds);

// Connection Helpers
void conn_init(connection_t *c, int fd, SSL *ssl);
void conn_close(connection_t *c);

// Wrapper for read/write
ssize_t conn_readn(connection_t *c, void *buf, size_t n);
ssize_t conn_writen(connection_t *c, const void *buf, size_t n);

// Legacy helpers (still used potentially)
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

// SSL Init Helpers
void ssl_msg_init(void); // Init lib
SSL_CTX* ssl_init_server_ctx(const char *cert_path, const char *key_path);
SSL_CTX* ssl_init_client_ctx(void);