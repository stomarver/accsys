#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#define UUID_LEN 19
#define MAX_INPUT_BUF 4096
#define HASH_SIZE 32
#define SALT_BYTES 16
#define SALT_HEX (SALT_BYTES * 2)
#define HASH_HEX (HASH_SIZE * 2)
/* output: salt_hex:xor_rotate_hex = 32 + 1 + 64 = 97 chars */

/* ---------- random ---------- */
static int seeded = 0;

static void ensure_seed(void) {
    if (!seeded) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        seeded = 1;
    }
}

static char random_hex_char(void) {
    int v = rand() % 16;
    if (v < 10) return '0' + v;
    return 'a' + (v - 10);
}

static int read_random_bytes(unsigned char *buf, size_t len) {
    int fd;
    ssize_t r;
    size_t got = 0;

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;

    while (got < len) {
        r = read(fd, buf + got, len - got);
        if (r <= 0) {
            close(fd);
            return 0;
        }
        got += (size_t)r;
    }

    close(fd);
    return 1;
}

static void generate_salt(unsigned char *salt_bytes, char *salt_hex) {
    size_t i;
    const char *hex = "0123456789abcdef";

    if (!read_random_bytes(salt_bytes, SALT_BYTES)) {
        /* fallback to rand if /dev/urandom unavailable */
        ensure_seed();
        for (i = 0; i < SALT_BYTES; i++) {
            *(salt_bytes + i) = (unsigned char)(rand() & 0xFF);
        }
    }

    for (i = 0; i < SALT_BYTES; i++) {
        *(salt_hex + i * 2) = *(hex + (*(salt_bytes + i) >> 4));
        *(salt_hex + i * 2 + 1) = *(hex + (*(salt_bytes + i) & 0x0F));
    }
    *(salt_hex + SALT_HEX) = '\0';
}

static int hex_to_byte(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_salt_hex(const char *salt_hex, unsigned char *salt_bytes) {
    size_t i;
    int hi, lo;

    if (strlen(salt_hex) != SALT_HEX) return 0;

    for (i = 0; i < SALT_BYTES; i++) {
        hi = hex_to_byte(*(salt_hex + i * 2));
        lo = hex_to_byte(*(salt_hex + i * 2 + 1));
        if (hi < 0 || lo < 0) return 0;
        *(salt_bytes + i) = (unsigned char)((hi << 4) | lo);
    }
    return 1;
}

/* ---------- uuid ---------- */
static void generate_uuid(char *out) {
    int i;
    int pos = 0;

    ensure_seed();

    for (i = 0; i < 4; i++) {
        int j;
        if (i > 0) {
            *(out + pos) = '-';
            pos++;
        }
        for (j = 0; j < 4; j++) {
            *(out + pos) = random_hex_char();
            pos++;
        }
    }
    *(out + pos) = '\0';
}

/* ---------- SHA-256 ---------- */

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_transform(uint32_t *state, const unsigned char *block) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)*(block + i * 4) << 24) |
        ((uint32_t)*(block + i * 4 + 1) << 16) |
        ((uint32_t)*(block + i * 4 + 2) << 8) |
        ((uint32_t)*(block + i * 4 + 3));
    }

    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = *(state + 0); b = *(state + 1);
    c = *(state + 2); d = *(state + 3);
    e = *(state + 4); f = *(state + 5);
    g = *(state + 6); h = *(state + 7);

    for (i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    *(state + 0) += a; *(state + 1) += b;
    *(state + 2) += c; *(state + 3) += d;
    *(state + 4) += e; *(state + 5) += f;
    *(state + 6) += g; *(state + 7) += h;
}

static void sha256(const unsigned char *data, size_t len, unsigned char *out) {
    uint32_t state[8];
    unsigned char block[64];
    size_t i;
    size_t remaining;
    uint64_t bits;

    *(state + 0) = 0x6a09e667u; *(state + 1) = 0xbb67ae85u;
    *(state + 2) = 0x3c6ef372u; *(state + 3) = 0xa54ff53au;
    *(state + 4) = 0x510e527fu; *(state + 5) = 0x9b05688cu;
    *(state + 6) = 0x1f83d9abu; *(state + 7) = 0x5be0cd19u;

    for (i = 0; i + 64 <= len; i += 64) {
        sha256_transform(state, data + i);
    }

    remaining = len - i;
    memset(block, 0, 64);
    if (remaining > 0) memcpy(block, data + i, remaining);
    *(block + remaining) = 0x80;

    if (remaining >= 56) {
        sha256_transform(state, block);
        memset(block, 0, 64);
    }

    bits = (uint64_t)len * 8;
    *(block + 56) = (unsigned char)(bits >> 56);
    *(block + 57) = (unsigned char)(bits >> 48);
    *(block + 58) = (unsigned char)(bits >> 40);
    *(block + 59) = (unsigned char)(bits >> 32);
    *(block + 60) = (unsigned char)(bits >> 24);
    *(block + 61) = (unsigned char)(bits >> 16);
    *(block + 62) = (unsigned char)(bits >> 8);
    *(block + 63) = (unsigned char)(bits);

    sha256_transform(state, block);

    for (i = 0; i < 8; i++) {
        *(out + i * 4) = (unsigned char)(*(state + i) >> 24);
        *(out + i * 4 + 1) = (unsigned char)(*(state + i) >> 16);
        *(out + i * 4 + 2) = (unsigned char)(*(state + i) >> 8);
        *(out + i * 4 + 3) = (unsigned char)(*(state + i));
    }
}

/* ---------- key stretching: HMAC-SHA256 based PBKDF2-like ---------- */

static void hmac_sha256(const unsigned char *key, size_t key_len,
                        const unsigned char *msg, size_t msg_len,
                        unsigned char *out) {
    unsigned char ipad[64];
    unsigned char opad[64];
    unsigned char key_block[64];
    unsigned char inner[MAX_INPUT_BUF];
    unsigned char inner_hash[HASH_SIZE];
    unsigned char outer[64 + HASH_SIZE];
    size_t i;

    memset(key_block, 0, 64);
    if (key_len > 64) {
        sha256(key, key_len, key_block);
    } else {
        memcpy(key_block, key, key_len);
    }

    for (i = 0; i < 64; i++) {
        *(ipad + i) = *(key_block + i) ^ 0x36;
        *(opad + i) = *(key_block + i) ^ 0x5c;
    }

    memcpy(inner, ipad, 64);
    if (msg_len > 0 && msg_len < MAX_INPUT_BUF - 64) {
        memcpy(inner + 64, msg, msg_len);
    }
    sha256(inner, 64 + msg_len, inner_hash);

    memcpy(outer, opad, 64);
    memcpy(outer + 64, inner_hash, HASH_SIZE);
    sha256(outer, 64 + HASH_SIZE, out);
                        }

                        #define STRETCH_ROUNDS 10000

                        static void stretch_key(const unsigned char *password, size_t pass_len,
                                                const unsigned char *salt, size_t salt_len,
                                                unsigned char *out) {
                            unsigned char u[HASH_SIZE];
                            unsigned char t[HASH_SIZE];
                            unsigned char msg[MAX_INPUT_BUF];
                            size_t msg_len;
                            int i;
                            size_t j;

                            /* U1 = HMAC(password, salt || 0x00000001) */
                            msg_len = salt_len + 4;
                            memcpy(msg, salt, salt_len);
                            *(msg + salt_len) = 0;
                            *(msg + salt_len + 1) = 0;
                            *(msg + salt_len + 2) = 0;
                            *(msg + salt_len + 3) = 1;

                            hmac_sha256(password, pass_len, msg, msg_len, u);
                            memcpy(t, u, HASH_SIZE);

                            /* U2..Un = HMAC(password, U_prev), T ^= Ui */
                            for (i = 1; i < STRETCH_ROUNDS; i++) {
                                hmac_sha256(password, pass_len, u, HASH_SIZE, u);
                                for (j = 0; j < HASH_SIZE; j++) {
                                    *(t + j) ^= *(u + j);
                                }
                            }

                            memcpy(out, t, HASH_SIZE);
                                                }

                                                /* ---------- encryption layers (post-hash obfuscation) ---------- */

                                                static const char *XOR_KEY = "K7x#mP2$vL9@nQ4&";

                                                static void layer_xor(const unsigned char *in, size_t len, unsigned char *out) {
                                                    size_t key_len = strlen(XOR_KEY);
                                                    size_t i;
                                                    for (i = 0; i < len; i++) {
                                                        *(out + i) = *(in + i) ^ (unsigned char)*(XOR_KEY + (i % key_len));
                                                    }
                                                }

                                                static void layer_rotate(const unsigned char *in, size_t len, unsigned char *out) {
                                                    size_t i;
                                                    for (i = 0; i < len; i++) {
                                                        unsigned char b = *(in + i);
                                                        b = (unsigned char)((b << 3) | (b >> 5));
                                                        b = (unsigned char)(b + 47);
                                                        *(out + i) = b;
                                                    }
                                                }

                                                static void to_hex(const unsigned char *in, size_t len, char *out) {
                                                    size_t i;
                                                    const char *hex = "0123456789abcdef";
                                                    for (i = 0; i < len; i++) {
                                                        *(out + i * 2) = *(hex + (*(in + i) >> 4));
                                                        *(out + i * 2 + 1) = *(hex + (*(in + i) & 0x0F));
                                                    }
                                                    *(out + len * 2) = '\0';
                                                }

                                                /* ---------- full encrypt: returns "salt_hex:hash_hex" ---------- */
                                                static char *encrypt_password(const char *password) {
                                                    unsigned char salt_bytes[SALT_BYTES];
                                                    char salt_hex[SALT_HEX + 1];
                                                    unsigned char derived[HASH_SIZE];
                                                    unsigned char buf1[HASH_SIZE];
                                                    unsigned char buf2[HASH_SIZE];
                                                    char hash_hex[HASH_HEX + 1];
                                                    char *result;

                                                    if (strlen(password) == 0) {
                                                        result = malloc(1);
                                                        *result = '\0';
                                                        return result;
                                                    }

                                                    /* generate random salt */
                                                    generate_salt(salt_bytes, salt_hex);

                                                    /* PBKDF2-HMAC-SHA256, 10000 rounds */
                                                    stretch_key((const unsigned char *)password, strlen(password),
                                                                salt_bytes, SALT_BYTES, derived);

                                                    /* obfuscation layers */
                                                    layer_xor(derived, HASH_SIZE, buf1);
                                                    layer_rotate(buf1, HASH_SIZE, buf2);
                                                    to_hex(buf2, HASH_SIZE, hash_hex);

                                                    /* "salt_hex:hash_hex" */
                                                    result = malloc(SALT_HEX + 1 + HASH_HEX + 1);
                                                    memcpy(result, salt_hex, SALT_HEX);
                                                    *(result + SALT_HEX) = ':';
                                                    memcpy(result + SALT_HEX + 1, hash_hex, HASH_HEX + 1);

                                                    return result;
                                                }

                                                /* ---------- verify: given "salt_hex:hash_hex" and password ---------- */
                                                static int verify_password(const char *stored, const char *password) {
                                                    char salt_hex[SALT_HEX + 1];
                                                    char stored_hash[HASH_HEX + 1];
                                                    unsigned char salt_bytes[SALT_BYTES];
                                                    unsigned char derived[HASH_SIZE];
                                                    unsigned char buf1[HASH_SIZE];
                                                    unsigned char buf2[HASH_SIZE];
                                                    char computed_hash[HASH_HEX + 1];
                                                    size_t stored_len;

                                                    if (!stored || !password) return 0;

                                                    stored_len = strlen(stored);
                                                    if (stored_len != SALT_HEX + 1 + HASH_HEX) return 0;
                                                    if (*(stored + SALT_HEX) != ':') return 0;

                                                    memcpy(salt_hex, stored, SALT_HEX);
                                                    *(salt_hex + SALT_HEX) = '\0';

                                                    memcpy(stored_hash, stored + SALT_HEX + 1, HASH_HEX);
                                                    *(stored_hash + HASH_HEX) = '\0';

                                                    if (!parse_salt_hex(salt_hex, salt_bytes)) return 0;

                                                    stretch_key((const unsigned char *)password, strlen(password),
                                                                salt_bytes, SALT_BYTES, derived);

                                                    layer_xor(derived, HASH_SIZE, buf1);
                                                    layer_rotate(buf1, HASH_SIZE, buf2);
                                                    to_hex(buf2, HASH_SIZE, computed_hash);

                                                    /* constant-time comparison */
                                                    {
                                                        size_t i;
                                                        unsigned char diff = 0;
                                                        for (i = 0; i < HASH_HEX; i++) {
                                                            diff |= (unsigned char)(*(computed_hash + i) ^ *(stored_hash + i));
                                                        }
                                                        return diff == 0 ? 1 : 0;
                                                    }
                                                }

                                                /* ---------- key validation ---------- */
                                                static int is_valid_key(const char *s) {
                                                    size_t i;
                                                    size_t n;

                                                    if (!s) return 0;
                                                    n = strlen(s);
                                                    if (n == 0) return 0;

                                                    for (i = 0; i < n; i++) {
                                                        unsigned char c = (unsigned char)*(s + i);
                                                        if (c >= 'a' && c <= 'z') continue;
                                                        if (c >= 'A' && c <= 'Z') continue;
                                                        if (c >= '0' && c <= '9') continue;
                                                        if (c == '+' || c == '=' || c == '_' || c == '-' || c == '*') continue;
                                                        return 0;
                                                    }
                                                    return 1;
                                                }

                                                /* ---------- main ---------- */
                                                static void trim_nl(char *s) {
                                                    size_t n = strlen(s);
                                                    while (n && (*(s + n - 1) == '\n' || *(s + n - 1) == '\r')) {
                                                        --n;
                                                        *(s + n) = '\0';
                                                    }
                                                }

                                                int main(int argc, char **argv) {
                                                    if (argc < 2) {
                                                        fprintf(stderr, "usage:\n");
                                                        fprintf(stderr, "  generator uuid\n");
                                                        fprintf(stderr, "  generator encrypt <password>\n");
                                                        fprintf(stderr, "  generator verify <stored> <password>\n");
                                                        fprintf(stderr, "  generator validate <key>\n");
                                                        return 1;
                                                    }

                                                    if (strcmp(*(argv + 1), "uuid") == 0) {
                                                        char uuid[UUID_LEN + 1];
                                                        generate_uuid(uuid);
                                                        printf("%s\n", uuid);
                                                        return 0;
                                                    }

                                                    if (strcmp(*(argv + 1), "encrypt") == 0) {
                                                        char *encrypted;
                                                        char *password;
                                                        int need_free = 0;

                                                        if (argc >= 3) {
                                                            password = *(argv + 2);
                                                        } else {
                                                            password = malloc(MAX_INPUT_BUF);
                                                            if (!fgets(password, MAX_INPUT_BUF, stdin)) {
                                                                free(password);
                                                                return 1;
                                                            }
                                                            trim_nl(password);
                                                            need_free = 1;
                                                        }

                                                        if (!is_valid_key(password)) {
                                                            if (need_free) free(password);
                                                            fprintf(stderr, "invalid key\n");
                                                            return 1;
                                                        }

                                                        encrypted = encrypt_password(password);
                                                        printf("%s\n", encrypted);

                                                        free(encrypted);
                                                        if (need_free) free(password);
                                                        return 0;
                                                    }

                                                    if (strcmp(*(argv + 1), "verify") == 0) {
                                                        if (argc != 4) {
                                                            fprintf(stderr, "usage: generator verify <stored> <password>\n");
                                                            return 1;
                                                        }

                                                        if (verify_password(*(argv + 2), *(argv + 3))) {
                                                            return 0; /* match */
                                                        }
                                                        return 1; /* no match */
                                                    }

                                                    if (strcmp(*(argv + 1), "validate") == 0) {
                                                        char *key;
                                                        int need_free = 0;

                                                        if (argc >= 3) {
                                                            key = *(argv + 2);
                                                        } else {
                                                            key = malloc(MAX_INPUT_BUF);
                                                            if (!fgets(key, MAX_INPUT_BUF, stdin)) {
                                                                free(key);
                                                                return 1;
                                                            }
                                                            trim_nl(key);
                                                            need_free = 1;
                                                        }

                                                        if (is_valid_key(key)) {
                                                            printf("valid\n");
                                                            if (need_free) free(key);
                                                            return 0;
                                                        } else {
                                                            printf("invalid\n");
                                                            if (need_free) free(key);
                                                            return 1;
                                                        }
                                                    }

                                                    fprintf(stderr, "unknown command: %s\n", *(argv + 1));
                                                    return 1;
                                                }
