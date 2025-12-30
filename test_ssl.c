
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdio.h>

int main() {
    SSL_library_init();
    printf("OpenSSL loaded\n");
    return 0;
}
