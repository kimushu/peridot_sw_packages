#include "sys/alt_dev.h"
#include "sys/alt_llist.h"
#include "rubic_agent.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <errno.h>
#include <sys/fcntl.h>
#include <string.h>
#include <malloc.h>
#include "system.h"

/* Prototypes */
static int rubic_agent_open(alt_fd *fd, const char *name, int flags, int mode);
static int rubic_agent_close(alt_fd *fd);
static int rubic_agent_read(alt_fd *fd, char *ptr, int len);
static int rubic_agent_write(alt_fd *fd, const char *ptr, int len);

static alt_dev fs_dev = {
	ALT_LLIST_ENTRY,
	RUBIC_AGENT_ROOT_NAME,
	rubic_agent_open,
	rubic_agent_close,
	rubic_agent_read,
	rubic_agent_write,
	NULL, /* lseek */
	NULL, /* fstat */
	NULL, /* ioctl */
};

static const char *info_json =
"{"
	"\"rubic\":\"" RUBIC_AGENT_RUBIC_VERSION "\","
	"\"runtimes\":["
		"{"
			"\"name\":\"" RUBIC_AGENT_RUNTIME1_NAME "\","
			"\"version\":\"" RUBIC_AGENT_RUNTIME1_VERSION "\""
		"}"
#ifdef RUBIC_AGENT_RUNTIME2_PRESENT
		",{"
			"\"name\":\"" RUBIC_AGENT_RUNTIME2_NAME "\","
			"\"version\":\"" RUBIC_AGENT_RUNTIME2_VERSION "\""
		"}"
#endif
#ifdef RUBIC_AGENT_RUNTIME3_PRESENT
		",{"
			"\"name\":\"" RUBIC_AGENT_RUNTIME3_NAME "\","
			"\"version\":\"" RUBIC_AGENT_RUNTIME3_VERSION "\""
		"}"
#endif
	"]"
"}";

static void *(*intr_handler)(int reason);
static char *current_program;
static sem_t sem_start;

/*
 * Start Rubic agent
 */
void rubic_agent_init(void)
{
	alt_fs_reg(&fs_dev);
	intr_handler = NULL;
	current_program = NULL;
	sem_init(&sem_start, 0, 0 /* TODO: auto start */);
}

/*
 * Set handler for interrupt program
 * (Notice: handler may be called from another thread)
 */
void rubic_agent_set_interrupt_handler(void *(*handler)(int reason))
{
	intr_handler = handler;
}

/*
 * Returns program name to start
 * (This function will block a caller thread until receive start request)
 */
const char *rubic_agent_wait_start_request(void)
{
	while (!current_program) {
		sem_wait(&sem_start);
	}
	return current_program;
}

/*
 * Set current program name
 * (If name is NULL, it means that program has been finished)
 */
int rubic_agent_set_program(const char *name)
{
	char *old_program = current_program;
	if (old_program == name) {
		// No change
		return 0;
	}
	if (!old_program && name) {
		// Cannot start program by this function
		return -1;
	}
	char *new_program = name ? strdup(name) : NULL;
	current_program = new_program;
	free(old_program);
	return 0;
}

/* Handler for open() */
static int rubic_agent_open(alt_fd *fd, const char *name, int flags, int mode)
{
	int request = -1;
	rubic_agent_fddata *data;
	int is_write = (((flags & O_ACCMODE) + 1) & _FWRITE);

	name += strlen(fs_dev.name) + 1;
	if (strcmp(name, "info") == 0) {
		request = RUBIC_AGENT_REQ_INFO;
		if (is_write) {
			// Not writable
			return -EACCES;
		}
	} else if (strcmp(name, "run") == 0) {
		request = RUBIC_AGENT_REQ_RUN;
		if (is_write && current_program) {
			// Not writable when program is running
			return -EBUSY;
		}
	} else if (strcmp(name, "stop") == 0) {
		request = RUBIC_AGENT_REQ_STOP;
	}

	if (request < 0) {
		return -ENOENT;
	}

	data = (rubic_agent_fddata *)malloc(sizeof(*data) + FILENAME_MAX);
	if (!data) {
		return -ENOMEM;
	}
	data->request = request;
	data->data_ptr = 0;
	data->data_len = 0;
	data->data_max = FILENAME_MAX;
	fd->priv = (alt_u8 *)data;

	switch (data->request) {
	case RUBIC_AGENT_REQ_INFO:
		strncpy(data->data, info_json, data->data_max);
		break;
	case RUBIC_AGENT_REQ_RUN:
		// Return current program name
		strncpy(data->data, current_program, data->data_max);
		data->data_len = strnlen(data->data, data->data_max - 1) + 1;
		break;
	case RUBIC_AGENT_REQ_STOP:
		// No initial data
		break;
	}
	return 0;
}

/*
 * Handler for close()
 */
static int rubic_agent_close(alt_fd *fd)
{
	rubic_agent_fddata *data = (rubic_agent_fddata *)fd->priv;

	if (!(((fd->fd_flags & O_ACCMODE) + 1) & _FWRITE)) {
		goto cleanup;
	}

	// When writing end
	switch (data->request) {
	case RUBIC_AGENT_REQ_RUN:
		if (current_program) {
			// FIXME
			break;
		}
		current_program = strndup(data->data, data->data_len);
		sem_post(&sem_start);
		break;
	case RUBIC_AGENT_REQ_STOP:
		if (intr_handler) {
			intr_handler(0 /* TODO: reason */);
		}
		break;
	}

cleanup:
	fd->priv = NULL;
	free(data);
	return 0;
}

static int rubic_agent_read(alt_fd *fd, char *ptr, int len)
{
	rubic_agent_fddata *data = (rubic_agent_fddata *)fd->priv;
	int read_len = data->data_len - data->data_ptr;
	if (len < read_len) {
		read_len = len;
	}
	if (read_len > 0) {
		memcpy(ptr, data->data + data->data_ptr, read_len);
		data->data_ptr += read_len;
	}
	return read_len;
}

static int rubic_agent_write(alt_fd *fd, const char *ptr, int len)
{
	rubic_agent_fddata *data = (rubic_agent_fddata *)fd->priv;
	int write_len = data->data_max - data->data_ptr;
	if (len < write_len) {
		write_len = len;
	}
	if (write_len > 0) {
		memcpy(data->data + data->data_ptr, ptr, write_len);
		data->data_ptr += write_len;
	} else if (len > 0) {
		return -ENOSPC;
	}
	data->data_len = data->data_ptr;
	return write_len;
}
