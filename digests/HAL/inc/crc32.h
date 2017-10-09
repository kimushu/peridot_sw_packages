#ifndef __CRC32_H__
#define __CRC32_H__

#include <stdint.h>

typedef uint32_t digest_crc32_t;

extern void digest_crc32_init(void);
extern void digest_crc32_calc(digest_crc32_t *result, const void *ptr, int len);

#endif  /* __CRC32_H__ */
