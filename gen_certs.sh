#!/bin/bash
# Generate self-signed certificate for testing

# 1. Generate Private Key
openssl genrsa -out server.key 2048

# 2. Generate Self-Signed Certificate
openssl req -new -x509 -key server.key -out server.crt -days 365 \
    -subj "/C=TW/ST=Taiwan/L=Taipei/O=TCG/OU=Game/CN=localhost"

# 3. (Optional) Generate Client Key/Cert if we want mutual auth (skipping for this simple case)

echo "Generated server.key and server.crt"
chmod 600 server.key
