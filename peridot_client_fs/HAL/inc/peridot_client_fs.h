#ifndef __PERIDOT_CLIENT_FS_H__
#define __PERIDOT_CLIENT_FS_H__

#include "alt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct peridot_client_fs_path_entry_s {
	struct peridot_client_fs_path_entry_s *next;
	int flags;
	int len;
	const char *name;
} peridot_client_fs_path_entry;

extern void peridot_client_fs_init(const char *rw_path, const char *ro_path, const char *wo_path);
extern void peridot_client_fs_add_file(const char *path, int flags);
extern void peridot_client_fs_add_directory(const char *path, int flags);

#define PERIDOT_CLIENT_FS_INSTANCE(name, state) extern int alt_no_storage
#define PERIDOT_CLIENT_FS_INIT(name, state) \
	peridot_client_fs_init( \
		PERIDOT_CLIENT_FS_RW_PATH, \
		PERIDOT_CLIENT_FS_RO_PATH, \
		PERIDOT_CLIENT_FS_WO_PATH \
	)

#ifdef __cplusplus
}	/* extern "C" */
#endif

#endif  /* __PERIDOT_CLIENT_FS_H__ */
