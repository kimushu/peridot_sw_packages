#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "os/alt_sem.h"
#include "sys/alt_irq.h"
#include "peridot_client_fs.h"
#include "peridot_rpc_server.h"
#include "bson.h"
#include "system.h"

#ifndef O_DIRECTORY
# define O_DIRECTORY 0x200000
#endif

#if defined(PERIDOT_CLIENT_FS_HASH_MD5) || defined(PERIDOT_CLIENT_FS_HASH_CRC32)
# define PERIDOT_CLIENT_FS_ENABLE_HASH 1
# include "digests.h"
#endif

ALT_STATIC_SEM(sem_lock);

static peridot_client_fs_path_entry *ll_first;
static peridot_client_fs_path_entry *ll_last;

enum {
	VFD_FREE     = -1,
	VFD_ALLOC    = -2,
	VFD_CANCELED = -3,
};
static int vfd_list[PERIDOT_CLIENT_FS_MAX_FDS];

static int peridot_client_fs_alloc_vfd(void)
{
	int vfd;

	ALT_SEM_PEND(sem_lock, 0);
	for (vfd = 0; vfd < (sizeof(vfd_list) / sizeof(*vfd_list)); ++vfd) {
		if (vfd_list[vfd] == VFD_FREE) {
			vfd_list[vfd] = VFD_ALLOC;
			ALT_SEM_POST(sem_lock);
			return vfd;
		}
	}
	ALT_SEM_POST(sem_lock);
	return -1;
}

static void peridot_client_fs_free_vfd(int vfd)
{
	vfd_list[vfd] = VFD_FREE;
}

static int peridot_client_fs_validate_path(const char *path, int flags)
{
	const peridot_client_fs_path_entry *entry;
	int len;
	int acc_requested = (flags & O_ACCMODE) + 1;

	len = strlen(path);
	if (len == 0) {
		return 0;
	}
	if (path[len - 1] == '/') {
		// Remove slash(/) at the end of path
		flags |= O_DIRECTORY;
		--len;
	}

	for (entry = ll_first; entry; entry = entry->next) {
		int acc_granted;
		char sep;

		if (len < entry->len) {
			continue;
		}

		if (memcmp(entry->name, path, entry->len) != 0) {
			continue;
		}

		// path forward match

		acc_granted = (entry->flags & O_ACCMODE) + 1;
		if ((acc_granted & acc_requested) != acc_requested) {
			return 0; // Prohibited access
		}

		sep = path[entry->len];
		if (entry->flags & O_DIRECTORY) {
			if ((sep == '/') || (sep == '\0')) {
				return 1;
			}
		} else {
			if (sep == '\0') {
				return 1;
			}
		}
	}

	// not found
	return 0;
}

/*
 * method: "fs.open"
 * params: {
 *   path: <string>  // path to file
 *   flags: <int32>  // combination of O_xx flags
 *   mode: <int32>   // [optional] permission (when O_CREAT)
 * }
 * result: {
 *   fd: <int32>     // file descriptor
 * }
 */
static void *peridot_client_fs_open(const void *params)
{
	int off_path;
	int off_flags;
	int off_mode;
	const char *path;
	int flags;
	int mode = 0777;
	int vfd;
	int fd;
	int result_len;
	void *result;
	alt_irq_context context;

	if (bson_get_props(params,
			"path", &off_path,
			"flags", &off_flags,
			"mode", &off_mode,
			NULL) < 0) {
		errno = JSONRPC_ERR_INVALID_REQUEST;
		return NULL;
	}

	if (!(path = bson_get_string(params, off_path, NULL))) {
		goto invalid_params;
	}

	if ((flags = bson_get_int32(params, off_flags, -1)) == -1) {
		goto invalid_params;
	}

	if ((off_mode >= 0) &&
		((mode = bson_get_int32(params, off_mode, -1)) == -1)) {
		goto invalid_params;
	}

	if (!peridot_client_fs_validate_path(path, flags)) {
		errno = EACCES;
		return NULL;
	}

	result_len = bson_empty_size + bson_measure_int32("fd");
	result = malloc(result_len);
	if (!result) {
		errno = ENOMEM;
		return NULL;
	}

	vfd = peridot_client_fs_alloc_vfd();
	if (vfd < 0) {
		free(result);
		errno = EMFILE;
		return NULL;
	}

	fd = open(path, flags, mode);
	if (fd < 0) {
		// errno already set
		int errno_saved = errno;
		free(result);
		peridot_client_fs_free_vfd(vfd);
		errno = errno_saved;
		return NULL;
	}

	context = alt_irq_disable_all();
	if (vfd_list[vfd] == VFD_ALLOC) {
		vfd_list[vfd] = fd;
	} else {
		vfd_list[vfd] = VFD_FREE;
		vfd = -1;
	}
	alt_irq_enable_all(context);
	if (vfd < 0) {
		free(result);
		close(fd);
		errno = EBADF;
		return NULL;
	}
	bson_create_empty_document(result);
	bson_set_int32(result, "fd", vfd);
	errno = 0;
	return result;

invalid_params:
	errno = JSONRPC_ERR_INVALID_PARAMS;
	return NULL;
}

static int peridot_client_fs_get_fd(const void *params, int off_vfd, int *vfd_ptr)
{
	int vfd;
	int fd;

	if (off_vfd < 0) {
		errno = JSONRPC_ERR_INVALID_PARAMS;
		return -1;
	}

	if (((vfd = bson_get_int32(params, off_vfd, -1)) < 0) ||
		(vfd >= (sizeof(vfd_list) / sizeof(*vfd_list))) ||
		((fd = vfd_list[vfd]) < 0)) {
		errno = EBADF;
		return -1;
	}

	if (vfd_ptr) {
		*vfd_ptr = vfd;
	}
	return fd;
}

/*
 * method: "fs.close"
 * params: {
 *   fd: <int32> // file descriptor
 * }
 * result: null
 */
static void *peridot_client_fs_close(const void *params)
{
	int off_vfd;
	int vfd;
	int fd;

	if (bson_get_props(params, "fd", &off_vfd, NULL) < 0) {
		errno = JSONRPC_ERR_INVALID_REQUEST;
		return NULL;
	}

	if ((fd = peridot_client_fs_get_fd(params, off_vfd, &vfd)) < 0) {
		// errno already set
		return NULL;
	}

	vfd_list[vfd] = VFD_ALLOC;
	if (close(fd) < 0) {
		// errno already set
		vfd_list[vfd] = fd;
		return NULL;
	}

	peridot_client_fs_free_vfd(vfd);
	errno = 0;
	return NULL;
}

/*
 * method: "fs.read"
 * params: {
 *   fd: <int32>     // file descriptor
 *   length: <int32> // read length
 * }
 * result: {
 *   data: <binary>  // binary data (subtype: generic)
 *   length: <int32> // length (may be smaller than binary data)
 * }
 */
static void *peridot_client_fs_read(const void *params)
{
	int off_vfd;
	int off_len;
	int len;
	int fd;
	int result_len;
	void *result;
	int read_len;
	void *buf;

	if (bson_get_props(params,
			"fd", &off_vfd,
			"length", &off_len,
			NULL) < 0) {
		errno = JSONRPC_ERR_INVALID_REQUEST;
		return NULL;
	}

	if ((fd = peridot_client_fs_get_fd(params, off_vfd, NULL)) < 0) {
		// errno already set
		return NULL;
	}

	if ((len = bson_get_int32(params, off_len, -1)) < 0) {
		errno = JSONRPC_ERR_INVALID_PARAMS;
		return NULL;
	}

	result_len = bson_empty_size +
		bson_measure_binary("data", len) + bson_measure_int32("length");
	result = malloc(result_len);
	if (!result) {
		errno = ENOMEM;
		return NULL;
	}

	bson_create_empty_document(result);
	bson_set_binary_generic(result, "data", len, &buf);

	read_len = read(fd, buf, len);
	if (read_len < 0) {
		// errno already set
		int errno_saved = errno;
		free(result);
		errno = errno_saved;
		return NULL;
	}

	bson_set_int32(result, "length", read_len);
	return result;
}

#ifdef PERIDOT_CLIENT_FS_ENABLE_HASH
/*
 * method: "fs.hash"
 * params: {
 *   fd: <int32>        // file descriptor
 *   length: <int32>    // read length
 *   method: <string>   // hash algorithm
 * }
 * result: {
 *   hash: <binary>     // hash value
 *   length: <int32>    // length (may be smaller than binary data)
 * }
 */
static void *peridot_client_fs_hash(const void *params)
{
	int off_vfd;
	int off_len;
	int off_method;
	int len;
	int fd;
	const char *method;
	int result_len;
	void *result;
	int read_len;
	void *buf;

	if (bson_get_props(params,
			"fd", &off_vfd,
			"length", &off_len,
			"method", &off_method,
			NULL) < 0) {
		errno = JSONRPC_ERR_INVALID_REQUEST;
		return NULL;
	}

	if ((fd = peridot_client_fs_get_fd(params, off_vfd, NULL)) < 0) {
		// errno already set
		return NULL;
	}

	if ((len = bson_get_int32(params, off_len, -1)) < 0) {
		errno = JSONRPC_ERR_INVALID_PARAMS;
		return NULL;
	}

	buf = malloc(len);
	if (!buf) {
		errno = ENOMEM;
		return NULL;
	}

	read_len = read(fd, buf, len);
	if (read_len < 0) {
		// errno already set
		int errno_saved = errno;
		free(buf);
		errno = errno_saved;
		return NULL;
	}

	result_len = bson_empty_size + bson_measure_int32("length") + bson_measure_binary("hash", 32);
	result = malloc(result_len + len);
	if (!result) {
		free(buf);
		errno = ENOMEM;
		return NULL;
	}

	bson_create_empty_document(result);
	bson_set_int32(result, "length", read_len);

	method = bson_get_string(params, off_method, "");
	if (0) {
#ifdef PERIDOT_CLIENT_FS_HASH_MD5
	} else if (strcmp(method, "md5") == 0) {
		digest_md5_t *phash;
		bson_set_binary_generic(result, "hash", sizeof(*phash), (void **)&phash);
		digest_md5_calc(phash, buf, read_len);
#endif  /* PERIDOT_CLIENT_FS_HASH_MD5 */
#ifdef PERIDOT_CLIENT_FS_HASH_CRC32
	} else if (strcmp(method, "crc32") == 0) {
		digest_crc32_t *phash;
		bson_set_binary_generic(result, "hash", sizeof(*phash), (void **)&phash);
		digest_crc32_calc(phash, buf, read_len);
#endif  /* PERIDOT_CLIENT_FS_HASH_CRC32 */
	} else {
		// Unknown hash method
		free(buf);
		free(result);
		errno = JSONRPC_ERR_INVALID_PARAMS;
		return NULL;
	}

	free(buf);
	return result;
}
#endif  /* PERIDOT_CLIENT_FS_ENABLE_HASH */

/*
 * method: "fs.write"
 * params: {
 *   fd: <int32>     // file descriptor
 *   data: <binary>  // write data
 * }
 * result: {
 *   length: <int32> // length written (may be smaller than write data)
 * }
 */
static void *peridot_client_fs_write(const void *params)
{
	int off_vfd;
	int off_data;
	int fd;
	const void *buf;
	int binlen;
	int result_len;
	void *result;
	int written_len;

	if (bson_get_props(params,
			"fd", &off_vfd,
			"data", &off_data,
			NULL) < 0) {
		errno = JSONRPC_ERR_INVALID_REQUEST;
		return NULL;
	}

	if ((fd = peridot_client_fs_get_fd(params, off_vfd, NULL)) < 0) {
		// errno already set
		return NULL;
	}

	if (!(buf = bson_get_binary(params, off_data, &binlen))) {
		errno = JSONRPC_ERR_INVALID_PARAMS;
		return NULL;
	}

	result_len = bson_empty_size + bson_measure_int32("length");
	result = malloc(result_len);
	if (!result) {
		errno = ENOMEM;
		return NULL;
	}

	written_len = write(fd, buf, binlen);
	if (written_len < 0) {
		// errno already set
		int errno_saved = errno;
		free(result);
		errno = errno_saved;
		return NULL;
	}

	bson_create_empty_document(result);
	bson_set_int32(result, "length", written_len);
	return result;
}

/*
 * method: "fs.lseek"
 * params: {
 *   fd: <int32>     // file descriptor
 *   offset: <int32> // offset bytes from 'whence'
 *   whence: <int32> // whence (SEEK_SET=0,SEEK_CUR=1,SEEK_END=2)
 * }
 * result: {
 *   offset: <int32> // offset bytes (from beginning of file) after seek
 * }
 */
static void *peridot_client_fs_lseek(const void *params)
{
	int off_vfd;
	int off_offset;
	int off_whence;
	int fd;
	int offset;
	int whence;
	int result_len;
	void *result;
	int offset_after;

	if (bson_get_props(params,
			"fd", &off_vfd,
			"offset", &off_offset,
			"whence", &off_whence,
			NULL) < 0) {
		errno = JSONRPC_ERR_INVALID_REQUEST;
		return NULL;
	}

	if ((fd = peridot_client_fs_get_fd(params, off_vfd, NULL)) < 0) {
		// errno already set
		return NULL;
	}

	offset = bson_get_int32(params, off_offset, 0);

	if ((whence = bson_get_int32(params, off_whence, -1)) < 0 || (whence > 2)) {
		errno = JSONRPC_ERR_INVALID_PARAMS;
		return NULL;
	}

	result_len = bson_empty_size + bson_measure_int32("offset");
	result = malloc(result_len);
	if (!result) {
		errno = ENOMEM;
		return NULL;
	}

	offset_after = lseek(fd, offset, whence);
	if (offset_after < 0) {
		// errno already set
		int errno_saved = errno;
		free(result);
		errno = errno_saved;
		return NULL;
	}

	bson_create_empty_document(result);
	bson_set_int32(result, "offset", offset_after);
	return result;
}

/*
 * method: "fs.ioctl"
 * params: {
 *   fd: <int32>    // file descriptor
 *   req: <int32>   // request number (must be a zero or positive number)
 *   arg: <binary>  // argument (can be null)
 * }
 * result: {
 *   result: <int32>    // return value from ioctl
 *   response: <binary> // response (modified arg data)
 * }
 */
static void *peridot_client_fs_ioctl(const void *params)
{
	int off_vfd;
	int off_req;
	int off_arg;
	int fd;
	int req;
	const void *arg_input;
	void *arg_output;
	int arg_len;
	int result_len;
	void *result;
	int return_value;

	if (bson_get_props(params,
			"fd", &off_vfd,
			"req", &off_req,
			"arg", &off_arg,
			NULL) < 0) {
		errno = JSONRPC_ERR_INVALID_REQUEST;
		return NULL;
	}

	if ((fd = peridot_client_fs_get_fd(params, off_vfd, NULL)) < 0) {
		// errno already set
		return NULL;
	}

	req = bson_get_int32(params, off_req, -1);
	arg_input = bson_get_binary(params, off_arg, &arg_len);

	if (req < 0) {
		errno = JSONRPC_ERR_INVALID_PARAMS;
		return NULL;
	}

	result_len = bson_empty_size + bson_measure_int32("result");
	if (arg_input) {
		result_len += bson_measure_binary("response", arg_len);
	} else {
		result_len += bson_measure_null("response");
	}
	result = malloc(result_len);
	if (!result) {
		errno = ENOMEM;
		return NULL;
	}

	bson_create_empty_document(result);
	if (arg_input) {
		bson_set_binary_generic(result, "response", arg_len, &arg_output);
		memcpy(arg_output, arg_input, arg_len);
	} else {
		bson_set_null(result, "response");
	}

	return_value = ioctl(fd, req, arg_output);
	if (return_value < 0) {
		// errno already set
		int errno_saved = errno;
		free(result);
		errno = errno_saved;
		return NULL;
	}

	bson_set_int32(result, "result", return_value);
	return result;
}

static const char *find_separator(const char *text)
{
	char ch;
	for (; (ch = *text) != '\0'; ++text) {
		if (ch == ':') {
			return text;
		}
	}
	return text;
}

static void peridot_client_fs_add_path(const char *path, int len, int flags, int copy)
{
	peridot_client_fs_path_entry *entry;

	entry = (peridot_client_fs_path_entry *)malloc(sizeof(*entry) + (copy ? len : 0));
	if (!entry) {
		return;
	}

	if (copy) {
		memcpy(entry + 1, path, len);
		entry->name = (const char *)(entry + 1);
	} else {
		entry->name = path;
	}

	entry->next = NULL;
	entry->len = len;
	entry->flags = flags & (O_ACCMODE | O_DIRECTORY);

	ALT_SEM_PEND(sem_lock, 0);
	if (ll_last) {
		ll_last->next = entry;
	} else {
		ll_first = entry;
	}
	ll_last = entry;
	ALT_SEM_POST(sem_lock);
}

static void peridot_client_fs_add_list(const char *list, int flags)
{
	const char *next;

	for (;;) {
		next = find_separator(list);
		if (next == list) {
			if (*next != '\0') {
				list = next + 1;
				continue;
			}
			break;
		}

		if (next[-1] == '/') {
			peridot_client_fs_add_path(list, next - list - 1, flags | O_DIRECTORY, 0);
		} else {
			peridot_client_fs_add_path(list, next - list, flags, 0);
		}

		if (*next == '\0') {
			break;
		}
		list = next + 1;
	}
}

/*
 * method: "fs.cleanup"
 * params: {
 *   // not used
 * }
 * result: null
 */
static void *peridot_client_fs_cleanup(const void *params)
{
	int vfd, fd;

	ALT_SEM_PEND(sem_lock, 0);
	for (vfd = 0; vfd < (sizeof(vfd_list) / sizeof(*vfd_list)); ++vfd) {
		alt_irq_context context = alt_irq_disable_all();
		fd = vfd_list[vfd];
		if (vfd_list[vfd] == VFD_ALLOC) {
			vfd_list[vfd] = VFD_CANCELED;
		}
		alt_irq_enable_all(context);
		if (fd >= 0) {
			vfd_list[vfd] = VFD_FREE;
			close(fd);
		}
	}
	ALT_SEM_POST(sem_lock);

	errno = 0;
	return NULL;
}

void peridot_client_fs_init(const char *rw_path, const char *ro_path, const char *wo_path)
{
	int i;
	for (i = 0; i < (sizeof(vfd_list) / sizeof(*vfd_list)); ++i) {
		vfd_list[i] = VFD_FREE;
	}

	ALT_SEM_CREATE(&sem_lock, 1);

	peridot_client_fs_add_list(rw_path, O_RDWR);
	peridot_client_fs_add_list(ro_path, O_RDONLY);
	peridot_client_fs_add_list(wo_path, O_WRONLY);

	peridot_rpc_server_register_sync_method("fs.open", peridot_client_fs_open);
	peridot_rpc_server_register_sync_method("fs.close", peridot_client_fs_close);
	peridot_rpc_server_register_sync_method("fs.read", peridot_client_fs_read);
#ifdef PERIDOT_CLIENT_FS_ENABLE_HASH
	peridot_rpc_server_register_sync_method("fs.hash", peridot_client_fs_hash);
#endif
	peridot_rpc_server_register_sync_method("fs.write", peridot_client_fs_write);
	peridot_rpc_server_register_sync_method("fs.lseek", peridot_client_fs_lseek);
	peridot_rpc_server_register_sync_method("fs.ioctl", peridot_client_fs_ioctl);
	peridot_rpc_server_register_sync_method("fs.cleanup", peridot_client_fs_cleanup);
}

void peridot_client_fs_add_file(const char *path, int flags)
{
	int len = strlen(path);
	peridot_client_fs_add_path(path, len, (flags & O_ACCMODE), 1);
}

void peridot_client_fs_add_directory(const char *path, int flags)
{
	int len = strlen(path);
	if (path[len - 1] == '/') {
		--len;
	}
	peridot_client_fs_add_path(path, len, (flags & O_ACCMODE) | O_DIRECTORY, 1);
}
