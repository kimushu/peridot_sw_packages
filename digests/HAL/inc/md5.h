#ifndef __MD5_H__
#define __MD5_H__

#include <stdint.h>

typedef struct {
    uint32_t words[4];
} digest_md5_t;

extern void digest_md5_init(void);
extern void digest_md5_calc(digest_md5_t *result, const void *ptr, int len);

#endif  /* __MD5_H__ */
