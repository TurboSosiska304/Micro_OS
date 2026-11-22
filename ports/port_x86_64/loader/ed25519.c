/*
 * Micro OS — Ed25519 (RFC 8032) over the arithmetic of TweetNaCl.
 *
 * TweetNaCl is in the public domain and small enough for a boot loader that
 * has no libc: the field is sixteen limbs of 16 bits, a point is four of them
 * in extended coordinates, and the hash is SHA-512. This file keeps the names
 * of the original where it can, because a reader who knows the reference
 * should recognise the code; the three public calls are Micro OS's own.
 */

#include "ed25519.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t i64;
typedef i64 gf[16];

static const gf GF0;
static const gf GF1 = {1};
static const gf D = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203,
};
static const gf D2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406,
};
static const gf X = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169,
};
static const gf Y = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
};
static const gf I = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83,
};

/* --- SHA-512, the hash inside the signature ----------------------------- */

static const u64 SHA512_K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

struct sha512_state {
    u64 h[8];
    u64 bytes;
    u8 block[128];
    u32 used;
};

static u64 rotr64(u64 value, unsigned bits)
{
    return (value >> bits) | (value << (64u - bits));
}

static void sha512_block(u64 state[8], const u8 *block)
{
    u64 w[80];
    u64 a;
    u64 b;
    u64 c;
    u64 d;
    u64 e;
    u64 f;
    u64 g;
    u64 h;
    unsigned i;

    for (i = 0; i < 16; i++) {
        w[i] = ((u64)block[8 * i] << 56) | ((u64)block[8 * i + 1] << 48) |
               ((u64)block[8 * i + 2] << 40) | ((u64)block[8 * i + 3] << 32) |
               ((u64)block[8 * i + 4] << 24) | ((u64)block[8 * i + 5] << 16) |
               ((u64)block[8 * i + 6] << 8) | (u64)block[8 * i + 7];
    }

    for (i = 16; i < 80; i++) {
        u64 s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        u64 s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];

    for (i = 0; i < 80; i++) {
        u64 s1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        u64 ch = (e & f) ^ (~e & g);
        u64 t1 = h + s1 + ch + SHA512_K[i] + w[i];
        u64 s0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        u64 maj = (a & b) ^ (a & c) ^ (b & c);
        u64 t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

static void sha512_init(struct sha512_state *state)
{
    static const u64 initial[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
    };
    unsigned i;

    for (i = 0; i < 8; i++) {
        state->h[i] = initial[i];
    }
    state->bytes = 0;
    state->used = 0;
}

static void sha512_update(struct sha512_state *state, const u8 *data, size_t len)
{
    size_t i;

    state->bytes += (u64)len;

    for (i = 0; i < len; i++) {
        state->block[state->used++] = data[i];
        if (state->used == 128) {
            sha512_block(state->h, state->block);
            state->used = 0;
        }
    }
}

static void sha512_final(struct sha512_state *state, u8 out[64])
{
    u64 bits = state->bytes * 8u;
    unsigned i;

    state->block[state->used++] = 0x80u;
    if (state->used > 112) {
        while (state->used < 128) {
            state->block[state->used++] = 0;
        }
        sha512_block(state->h, state->block);
        state->used = 0;
    }
    while (state->used < 112) {
        state->block[state->used++] = 0;
    }

    /* The 128-bit length: the top half is zero for every image of this tree. */
    for (i = 0; i < 8; i++) {
        state->block[112 + i] = 0;
    }
    for (i = 0; i < 8; i++) {
        state->block[120 + i] = (u8)(bits >> (56u - 8u * i));
    }
    sha512_block(state->h, state->block);

    for (i = 0; i < 8; i++) {
        out[8 * i] = (u8)(state->h[i] >> 56);
        out[8 * i + 1] = (u8)(state->h[i] >> 48);
        out[8 * i + 2] = (u8)(state->h[i] >> 40);
        out[8 * i + 3] = (u8)(state->h[i] >> 32);
        out[8 * i + 4] = (u8)(state->h[i] >> 24);
        out[8 * i + 5] = (u8)(state->h[i] >> 16);
        out[8 * i + 6] = (u8)(state->h[i] >> 8);
        out[8 * i + 7] = (u8)state->h[i];
    }
}

/* --- the field of sixteen limbs ----------------------------------------- */

static void set25519(gf target, const gf source)
{
    unsigned i;

    for (i = 0; i < 16; i++) {
        target[i] = source[i];
    }
}

static void car25519(gf value)
{
    int i;
    i64 carry;

    for (i = 0; i < 16; i++) {
        value[i] += 1LL << 16;
        carry = value[i] >> 16;
        value[(i + 1) * (i < 15)] += carry - 1 + 37 * (carry - 1) * (i == 15);
        value[i] -= carry * 65536;
    }
}

static void sel25519(gf p, gf q, int b)
{
    i64 mask = ~(i64)(b - 1);
    unsigned i;

    for (i = 0; i < 16; i++) {
        i64 t = mask & (p[i] ^ q[i]);

        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(u8 *out, const gf value)
{
    int i;
    int j;
    int borrow;
    gf m;
    gf t;

    for (i = 0; i < 16; i++) {
        t[i] = value[i];
    }
    car25519(t);
    car25519(t);
    car25519(t);

    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        borrow = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - borrow);
    }

    for (i = 0; i < 16; i++) {
        out[2 * i] = (u8)(t[i] & 0xff);
        out[2 * i + 1] = (u8)(t[i] >> 8);
    }
}

static int neq25519(const gf a, const gf b)
{
    u8 c[32];
    u8 d[32];
    unsigned i;

    pack25519(c, a);
    pack25519(d, b);

    i = 0;
    while (i < 32 && c[i] == d[i]) {
        i++;
    }

    return (int)(i < 32);
}

static u8 par25519(const gf a)
{
    u8 d[32];

    pack25519(d, a);

    return d[0] & 1;
}

static void unpack25519(gf out, const u8 *in)
{
    unsigned i;

    for (i = 0; i < 16; i++) {
        out[i] = in[2 * i] + ((i64)in[2 * i + 1] << 8);
    }
    out[15] &= 0x7fff;
}

static void field_add(gf out, const gf a, const gf b)
{
    unsigned i;

    for (i = 0; i < 16; i++) {
        out[i] = a[i] + b[i];
    }
}

static void field_sub(gf out, const gf a, const gf b)
{
    unsigned i;

    for (i = 0; i < 16; i++) {
        out[i] = a[i] - b[i];
    }
}

static void field_mul(gf out, const gf a, const gf b)
{
    i64 t[31];
    unsigned i;
    unsigned j;

    for (i = 0; i < 31; i++) {
        t[i] = 0;
    }

    for (i = 0; i < 16; i++) {
        for (j = 0; j < 16; j++) {
            t[i + j] += a[i] * b[j];
        }
    }

    for (i = 0; i < 15; i++) {
        t[i] += 38 * t[i + 16];
    }
    for (i = 0; i < 16; i++) {
        out[i] = t[i];
    }
    car25519(out);
    car25519(out);
}

static void field_square(gf out, const gf a)
{
    field_mul(out, a, a);
}

static void inv25519(gf out, const gf in)
{
    gf c;
    int a;

    for (a = 0; a < 16; a++) {
        c[a] = in[a];
    }
    for (a = 253; a >= 0; a--) {
        field_square(c, c);
        if (a != 2 && a != 4) {
            field_mul(c, c, in);
        }
    }
    for (a = 0; a < 16; a++) {
        out[a] = c[a];
    }
}

static void pow2523(gf out, const gf in)
{
    gf c;
    int a;

    for (a = 0; a < 16; a++) {
        c[a] = in[a];
    }
    for (a = 250; a >= 0; a--) {
        field_square(c, c);
        if (a != 1) {
            field_mul(c, c, in);
        }
    }
    for (a = 0; a < 16; a++) {
        out[a] = c[a];
    }
}

/* --- the points of the curve -------------------------------------------- */

static void point_add(gf p[4], gf q[4])
{
    gf a;
    gf b;
    gf c;
    gf d;
    gf e;
    gf f;
    gf g;
    gf h;
    gf t;

    field_sub(a, p[1], p[0]);
    field_sub(t, q[1], q[0]);
    field_mul(a, a, t);
    field_add(b, p[0], p[1]);
    field_add(t, q[0], q[1]);
    field_mul(b, b, t);
    field_mul(c, p[3], q[3]);
    field_mul(c, c, D2);
    field_mul(d, p[2], q[2]);
    field_add(d, d, d);
    field_sub(e, b, a);
    field_sub(f, d, c);
    field_add(g, d, c);
    field_add(h, b, a);

    field_mul(p[0], e, f);
    field_mul(p[1], h, g);
    field_mul(p[2], g, f);
    field_mul(p[3], e, h);
}

static void point_cswap(gf p[4], gf q[4], u8 b)
{
    unsigned i;

    for (i = 0; i < 4; i++) {
        sel25519(p[i], q[i], b);
    }
}

static void point_pack(u8 *out, gf p[4])
{
    gf tx;
    gf ty;
    gf zi;

    inv25519(zi, p[2]);
    field_mul(tx, p[0], zi);
    field_mul(ty, p[1], zi);
    pack25519(out, ty);
    out[31] ^= (u8)(par25519(tx) << 7);
}

static void point_scalarmult(gf p[4], gf q[4], const u8 *scalar)
{
    int i;

    set25519(p[0], GF0);
    set25519(p[1], GF1);
    set25519(p[2], GF1);
    set25519(p[3], GF0);

    for (i = 255; i >= 0; --i) {
        u8 bit = (u8)((scalar[i / 8] >> (i & 7)) & 1);

        point_cswap(p, q, bit);
        point_add(q, p);
        point_add(p, p);
        point_cswap(p, q, bit);
    }
}

static void point_scalarbase(gf p[4], const u8 *scalar)
{
    gf q[4];

    set25519(q[0], X);
    set25519(q[1], Y);
    set25519(q[2], GF1);
    field_mul(q[3], X, Y);
    point_scalarmult(p, q, scalar);
}

static int point_unpack_neg(gf r[4], const u8 p[32])
{
    gf t;
    gf chk;
    gf num;
    gf den;
    gf den2;
    gf den4;
    gf den6;

    set25519(r[2], GF1);
    unpack25519(r[1], p);
    field_square(num, r[1]);
    field_mul(den, num, D);
    field_sub(num, num, r[2]);
    field_add(den, r[2], den);

    field_square(den2, den);
    field_square(den4, den2);
    field_mul(den6, den4, den2);
    field_mul(t, den6, num);
    field_mul(t, t, den);

    pow2523(t, t);
    field_mul(t, t, num);
    field_mul(t, t, den);
    field_mul(t, t, den);
    field_mul(r[0], t, den);

    field_square(chk, r[0]);
    field_mul(chk, chk, den);
    if (neq25519(chk, num)) {
        field_mul(r[0], r[0], I);
    }

    field_square(chk, r[0]);
    field_mul(chk, chk, den);
    if (neq25519(chk, num)) {
        return -1;
    }

    if (par25519(r[0]) == (p[31] >> 7)) {
        field_sub(r[0], GF0, r[0]);
    }

    field_mul(r[3], r[0], r[1]);

    return 0;
}

/* --- the scalar modulo the order of the base point ---------------------- */

static const i64 ORDER[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10,
};

/* The bytes of the order, for the canonical check of a scalar. */
static const u8 ORDER_BYTES[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10,
};

static void scalar_mod_order(u8 *out, i64 x[64])
{
    i64 carry;
    int i;
    int j;

    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * ORDER[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }

    carry = 0;
    for (j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * ORDER[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; ++j) {
        x[j] -= carry * ORDER[j];
    }
    for (i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        out[i] = (u8)(x[i] & 255);
    }
}

static void scalar_reduce(u8 *hash)
{
    i64 x[64];
    unsigned i;

    for (i = 0; i < 64; ++i) {
        x[i] = (i64)hash[i];
    }
    for (i = 0; i < 64; ++i) {
        hash[i] = 0;
    }
    scalar_mod_order(hash, x);
}

static int scalar_is_canonical(const u8 scalar[32])
{
    int i;

    for (i = 31; i >= 0; --i) {
        if (scalar[i] < ORDER_BYTES[i]) {
            return 1;
        }
        if (scalar[i] > ORDER_BYTES[i]) {
            return 0;
        }
    }

    /* A scalar equal to the order is the same point as zero, and not
       canonical: a signature that says so is refused. */
    return 0;
}

static int bytes_differ(const u8 *a, const u8 *b, unsigned len)
{
    u32 difference = 0;
    unsigned i;

    for (i = 0; i < len; i++) {
        difference |= (u32)(a[i] ^ b[i]);
    }

    /* 0 when the two are the same, -1 otherwise, the answer of NaCl. */
    return (int)(1u & ((difference - 1u) >> 8)) - 1;
}

/* --- the three calls ---------------------------------------------------- */

void microos_ed25519_keypair(u8 public_key[32], u8 secret_key[64], const u8 seed[32])
{
    u8 digest[64];
    gf p[4];
    struct sha512_state hash;
    unsigned i;

    sha512_init(&hash);
    sha512_update(&hash, seed, 32);
    sha512_final(&hash, digest);
    digest[0] &= 248;
    digest[31] &= 127;
    digest[31] |= 64;

    point_scalarbase(p, digest);
    point_pack(public_key, p);

    for (i = 0; i < 32; i++) {
        secret_key[i] = seed[i];
        secret_key[32 + i] = public_key[i];
    }
}

int microos_ed25519_sign(u8 signature[64], const u8 *message, size_t len, const u8 secret_key[64])
{
    u8 digest[64];
    u8 nonce[64];
    u8 challenge[64];
    gf p[4];
    i64 x[64];
    struct sha512_state hash;
    unsigned i;
    unsigned j;

    sha512_init(&hash);
    sha512_update(&hash, secret_key, 32);
    sha512_final(&hash, digest);
    digest[0] &= 248;
    digest[31] &= 127;
    digest[31] |= 64;

    sha512_init(&hash);
    sha512_update(&hash, digest + 32, 32);
    if (len != 0) {
        sha512_update(&hash, message, len);
    }
    sha512_final(&hash, nonce);
    scalar_reduce(nonce);

    point_scalarbase(p, nonce);
    point_pack(signature, p);

    sha512_init(&hash);
    sha512_update(&hash, signature, 32);
    sha512_update(&hash, secret_key + 32, 32);
    if (len != 0) {
        sha512_update(&hash, message, len);
    }
    sha512_final(&hash, challenge);
    scalar_reduce(challenge);

    for (i = 0; i < 64; ++i) {
        x[i] = 0;
    }
    for (i = 0; i < 32; ++i) {
        x[i] = (i64)nonce[i];
    }
    for (i = 0; i < 32; ++i) {
        for (j = 0; j < 32; ++j) {
            x[i + j] += (i64)challenge[i] * (i64)digest[j];
        }
    }
    scalar_mod_order(signature + 32, x);

    return 0;
}

int microos_ed25519_verify(const u8 signature[64], const u8 *message, size_t len,
                           const u8 public_key[32])
{
    u8 challenge[64];
    u8 expected[32];
    gf p[4];
    gf q[4];
    struct sha512_state hash;

    if (!scalar_is_canonical(signature + 32)) {
        return -1;
    }

    /* unpack_neg gives -A; [S]B added to it is [S]B - [h]A, which has to be
       the R of the signature. */
    if (point_unpack_neg(q, public_key)) {
        return -1;
    }

    sha512_init(&hash);
    sha512_update(&hash, signature, 32);
    sha512_update(&hash, public_key, 32);
    if (len != 0) {
        sha512_update(&hash, message, len);
    }
    sha512_final(&hash, challenge);
    scalar_reduce(challenge);

    point_scalarmult(p, q, challenge);
    point_scalarbase(q, signature + 32);
    point_add(p, q);
    point_pack(expected, p);

    return bytes_differ(signature, expected, 32) ? -1 : 0;
}
