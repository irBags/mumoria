// Mumoria - password hashing and the AOL hand-off token.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MU_SALT_LEN 16
#define MU_HASH_LEN 32
#define MU_PBKDF2_ROUNDS 100000

void mu_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void mu_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32]);
void mu_pbkdf2(const char *password, const uint8_t *salt, size_t salt_len, int rounds, uint8_t out[MU_HASH_LEN]);

// Fills buf with random bytes from /dev/urandom; 0 on success.
int mu_random_bytes(uint8_t *buf, size_t len);

void mu_hex(const uint8_t *bytes, size_t len, char *out); // out: 2*len+1
int mu_unhex(const char *hex, uint8_t *out, size_t len);  // 0 on success

// Compares without leaking where they differ.
int mu_equal(const uint8_t *a, const uint8_t *b, size_t len);

// "name:expiry:hexmac" - 1 and the name if valid and unexpired.
int mu_token_check(const char *token, const char *secret, long now, char *name_out, size_t name_max);
// Builds a token (for tests and for the P3OL side).
void mu_token_make(const char *name, long expiry, const char *secret, char *out, size_t max);

#ifdef __cplusplus
}
#endif
