// Mumoria - SHA-256 (FIPS 180-4), HMAC (RFC 2104), PBKDF2 (RFC 8018).

#include "auth.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint32_t h[8];
    uint8_t block[64];
    size_t used;
    uint64_t total;
} Sha256;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha256 *c, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t) p[i * 4] << 24 | (uint32_t) p[i * 4 + 1] << 16 | (uint32_t) p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
    c->h[5] += f;
    c->h[6] += g;
    c->h[7] += h;
}

static void sha_init(Sha256 *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof(iv));
    c->used = 0;
    c->total = 0;
}

static void sha_update(Sha256 *c, const uint8_t *data, size_t len) {
    c->total += len;
    while (len > 0) {
        size_t n = 64 - c->used;
        if (n > len) {
            n = len;
        }
        memcpy(c->block + c->used, data, n);
        c->used += n;
        data += n;
        len -= n;
        if (c->used == 64) {
            sha_block(c, c->block);
            c->used = 0;
        }
    }
}

static void sha_final(Sha256 *c, uint8_t out[32]) {
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    sha_update(c, &pad, 1);
    pad = 0;
    while (c->used != 56) {
        sha_update(c, &pad, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (uint8_t) (bits >> (56 - 8 * i));
    }
    sha_update(c, len, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t) (c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t) (c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t) (c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t) c->h[i];
    }
}

void mu_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    Sha256 c;
    sha_init(&c);
    sha_update(&c, data, len);
    sha_final(&c, out);
}

void mu_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32]) {
    uint8_t k[64] = {0};
    if (key_len > 64) {
        mu_sha256(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    uint8_t inner[32];
    Sha256 c;
    sha_init(&c);
    sha_update(&c, ipad, 64);
    sha_update(&c, msg, msg_len);
    sha_final(&c, inner);
    sha_init(&c);
    sha_update(&c, opad, 64);
    sha_update(&c, inner, 32);
    sha_final(&c, out);
}

// PBKDF2-HMAC-SHA256 with a single output block (dkLen = 32).
void mu_pbkdf2(const char *password, const uint8_t *salt, size_t salt_len, int rounds, uint8_t out[MU_HASH_LEN]) {
    size_t plen = strlen(password);
    uint8_t msg[MU_SALT_LEN + 4 + 64];
    if (salt_len > MU_SALT_LEN + 64) {
        salt_len = MU_SALT_LEN + 64;
    }
    memcpy(msg, salt, salt_len);
    msg[salt_len] = 0;
    msg[salt_len + 1] = 0;
    msg[salt_len + 2] = 0;
    msg[salt_len + 3] = 1;
    uint8_t u[32];
    mu_hmac_sha256((const uint8_t *) password, plen, msg, salt_len + 4, u);
    memcpy(out, u, 32);
    for (int i = 1; i < rounds; i++) {
        mu_hmac_sha256((const uint8_t *) password, plen, u, 32, u);
        for (int j = 0; j < 32; j++) {
            out[j] ^= u[j];
        }
    }
}

int mu_random_bytes(uint8_t *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        got += (size_t) n;
    }
    close(fd);
    return 0;
}

void mu_hex(const uint8_t *bytes, size_t len, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 15];
    }
    out[len * 2] = '\0';
}

static int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int mu_unhex(const char *hex, uint8_t *out, size_t len) {
    if (strlen(hex) != len * 2) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        int hi = nibble(hex[i * 2]), lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t) (hi << 4 | lo);
    }
    return 0;
}

int mu_equal(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

static void token_mac(const char *name, long expiry, const char *secret, uint8_t mac[32]) {
    char msg[128];
    int n = snprintf(msg, sizeof(msg), "%s:%ld", name, expiry);
    mu_hmac_sha256((const uint8_t *) secret, strlen(secret), (const uint8_t *) msg, (size_t) n, mac);
}

void mu_token_make(const char *name, long expiry, const char *secret, char *out, size_t max) {
    uint8_t mac[32];
    char hex[65];
    token_mac(name, expiry, secret, mac);
    mu_hex(mac, 32, hex);
    snprintf(out, max, "%s:%ld:%s", name, expiry, hex);
}

int mu_token_check(const char *token, const char *secret, long now, char *name_out, size_t name_max) {
    if (secret == NULL || secret[0] == '\0') {
        return 0;
    }
    const char *c1 = strchr(token, ':');
    if (c1 == NULL || c1 == token || (size_t) (c1 - token) >= name_max || (size_t) (c1 - token) > 32) {
        return 0;
    }
    const char *c2 = strchr(c1 + 1, ':');
    if (c2 == NULL) {
        return 0;
    }
    char name[33];
    memcpy(name, token, (size_t) (c1 - token));
    name[c1 - token] = '\0';
    long expiry = strtol(c1 + 1, NULL, 10);
    if (expiry < now) {
        return 0;
    }
    uint8_t want[32], got[32];
    if (mu_unhex(c2 + 1, got, 32) != 0) {
        return 0;
    }
    token_mac(name, expiry, secret, want);
    if (!mu_equal(want, got, 32)) {
        return 0;
    }
    snprintf(name_out, name_max, "%s", name);
    return 1;
}
