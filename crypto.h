#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    unsigned long long bitlen;
    uint32_t state[8];
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t *hash);

/* Generates a 32-byte HMAC-SHA256 signature */
void hmac_sha256(const uint8_t *key, size_t keylen, const uint8_t *data, size_t datalen, uint8_t *mac);

#endif