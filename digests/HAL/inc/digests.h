#ifndef __DIGESTS_H__
#define __DIGESTS_H__

#include "system.h"
#include "md5.h"
#include "crc32.h"

#if defined(DIGESTS_MD5_ENABLE) && !defined(DIGESTS_MD5_STATIC_TABLE)
# define DIGESTS_MD5_DYNAMIC_INIT   digest_md5_init();
#else
# define DIGESTS_MD5_DYNAMIC_INIT
#endif

#if defined(DIGESTS_CRC32_ENABLE) && !defined(DIGESTS_CRC32_STATIC_TABLE)
# define DIGESTS_CRC32_DYNAMIC_INIT digest_crc32_init();
#else
# define DIGESTS_CRC32_DYNAMIC_INIT
#endif

#define DIGESTS_INSTANCE(name, state)   extern int alt_no_storage;
#define DIGESTS_INIT(name, state)   \
    do {                            \
        DIGESTS_MD5_DYNAMIC_INIT    \
        DIGESTS_CRC32_DYNAMIC_INIT  \
    } while (0)

#endif  /* __DIGESTS_H__ */
