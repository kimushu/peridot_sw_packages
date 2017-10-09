#include "md5.h"
#include "system.h"
#ifdef DIGESTS_MD5_ENABLE

#include <string.h>

static const uint8_t md5_table1[] = {
    7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
    5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
    4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
    6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,
};

#ifdef DIGESTS_MD5_STATIC_TABLE
static const uint32_t md5_table2[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
#else   /* !DIGESTS_MD5_STATIC_TABLE */
# include <math.h>
static uint32_t md5_table2[64];

void digest_md5_init(void)
{
    int i;
    for (i = 0; i < 64; ++i) {
        md5_table2[i] = (uint32_t)floor(fabs(sin(i + 1)) * 4294967296.0);
    }
}
#endif  /* !DIGESTS_MD5_STATIC_TABLE */

static uint32_t left_rotate(uint32_t value, int shift)
{
    return (value << shift) | (value >> (32 - shift));
}

void digest_md5_calc(digest_md5_t *result, const void *ptr, int len)
{
    const uint8_t *buf = (const uint8_t *)ptr;
    uint8_t temp[64];
    int chunks = (len + 8 + 64) >> 6;
    int total_bits = len << 3;

    uint32_t a0 = 0x67452301;
    uint32_t b0 = 0xefcdab89;
    uint32_t c0 = 0x98badcfe;
    uint32_t d0 = 0x10325476;

    for (; chunks > 0; --chunks, len -= 64) {
        uint32_t A = a0;
        uint32_t B = b0;
        uint32_t C = c0;
        uint32_t D = d0;
        uint32_t F;
        int g;
        const uint32_t *input;
        int i;
        
        if (len >= 64) {
            // This chunk contains input bits only
            input = (const uint32_t *)buf;
            buf += 64;
        } else {
            memset(temp, 0, 64);
            if (len >= 0) {
                // This chunk contains inputs bits and/or padding bits
                memcpy(temp, buf, len);
                temp[len] = 0x80;
            }
            if (len < 56) {
                temp[56] = (total_bits >>  0) & 0xff;
                temp[57] = (total_bits >>  8) & 0xff;
                temp[58] = (total_bits >> 16) & 0xff;
                temp[59] = (total_bits >> 24) & 0xff;
            }
            input = (const uint32_t *)temp;
        }

        for (i = 0; i < 64; ++i) {
            if (i < 16) {
                F = (B & C) | ((~B) & D);
                g = i;
            } else if (i < 32) {
                F = (D & B) | ((~D) & C);
                g = (5 * i + 1) & 15;
            } else if (i < 48) {
                F = B ^ C ^ D;
                g = (3 * i + 5) & 15;
            } else {
                F = C ^ (B | (~D));
                g = (7 * i) & 15;
            }
            uint32_t dTemp = D;
            D = C;
            C = B;
            B = B + left_rotate(A + F + md5_table2[i] + input[g], md5_table1[i]);
            A = dTemp;
        }
        a0 += A;
        b0 += B;
        c0 += C;
        d0 += D;
    }

    result->words[0] = a0;
    result->words[1] = b0;
    result->words[2] = c0;
    result->words[3] = d0;
}
#endif  /* DIGESTS_MD5_ENABLE */
