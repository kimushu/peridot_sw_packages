#include "system.h"
#include <stddef.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include "sys/alt_cache.h"
#include "peridot_rpc_server.h"
#include "peridot_sw_hostbridge_gen2.h"
#if (PERIDOT_RPCSRV_WORKER_THREADS) > 0
# define PERIDOT_RPCSRV_MULTI_THREAD
# include <pthread.h>
# include <semaphore.h>
#else
# undef PERIDOT_RPCSRV_MULTI_THREAD
#endif
#include "bson.h"

typedef struct rpcsrv_job_s {
    struct rpcsrv_job_s *next;
    union {
        alt_u8 bytes[0];
        alt_u32 words[0];
    } data;
} rpcsrv_job;

static struct {
	hostbridge_channel channel;
    alt_u8 escape_prefix;
    alt_u8 eop_prefix;
    alt_u8 inside_packet;
	size_t offset;
	union {
		alt_u8 bytes[4];
		alt_32 value;
	} length;
    rpcsrv_job *incoming_job;
    rpcsrv_job *volatile pending_job;
#ifdef PERIDOT_RPCSRV_MULTI_THREAD
    sem_t sem;
#endif
    peridot_rpc_server_method_entry *method_first, *method_last;
} state;

/*
 * Find method entry
 */
static peridot_rpc_server_method_entry *peridot_rpc_server_find_method(const char *name)
{
	peridot_rpc_server_method_entry *entry = state.method_first;

	for (; entry; entry = entry->next) {
		if (strcmp(entry->name, name) == 0) {
			return entry;
		}
	}

	return NULL;
}

/**
 * @func peridot_rpc_server_sink
 * @brief Stream sink function for RPC server
 */
static int peridot_rpc_server_sink(hostbridge_channel *channel, const void *ptr, int len)
{
    const alt_u8 *src = (const alt_u8 *)ptr;
    int read_len = 0;

    while ((read_len < len) && (!state.pending_job)) {
        alt_u8 byte = *src++;
        ++read_len;
        switch (byte) {
        case AST_SOP:
            state.offset = 0;
            state.inside_packet = 1;
            continue;
        case AST_EOP_PREFIX:
            state.eop_prefix = 1;
            continue;
        case AST_ESCAPE_PREFIX:
            state.escape_prefix = 1;
            continue;
        }
        if (state.escape_prefix) {
            byte ^= AST_ESCAPE_XOR;
            state.escape_prefix = 0;
        }
        if (!state.inside_packet) {
            continue;
        }
        if (state.offset < 4) {
            state.length.bytes[state.offset++] = byte;
            if (state.offset == 4) {
                if (state.length.value <= PERIDOT_RPCSRV_MAX_REQUEST_LENGTH) {
                    // Allocate buffer
                    state.incoming_job = (rpcsrv_job *)malloc(sizeof(rpcsrv_job) + state.length.value);
                    if (state.incoming_job) {
                        // Copy length
                        state.incoming_job->next = NULL;
                        state.incoming_job->data.words[0] = state.length.value;
                    }
                }
            }
        } else if (state.offset >= state.length.value) {
            // Packet data too large
drop_packet:
            free(state.incoming_job);
next_packet:
            state.incoming_job = NULL;
            state.offset = 0;
            state.eop_prefix = 0;
            state.inside_packet = 0;
            continue;
        } else if (state.incoming_job) {
            state.incoming_job->data.bytes[state.offset++] = byte;
        }
        if (!state.eop_prefix) {
            continue;
        }
        if (state.offset != state.length.value) {
            // Invalid packet size => drop packet
            goto drop_packet;
        }
        state.pending_job = state.incoming_job;
        state.incoming_job = NULL;
#ifdef PERIDOT_RPCSRV_MULTI_THREAD
        sem_post(&state.sem);
#endif
        goto next_packet;
    }

    return read_len;
}

#ifdef PERIDOT_RPCSRV_MULTI_THREAD
/*
 * Worker for server operations
 */
static void *peridot_rpc_server_worker(void *param)
{
    (void)param;
    for (;;) {
        peridot_rpc_server_service();
    }
    return NULL;
}
#endif  /* PERIDOT_RPCSRV_MULTI_THREAD */

/*
 * Initialize RPC server
 */
int peridot_rpc_server_init(void)
{
#ifdef PERIDOT_RPCSRV_MULTI_THREAD
	int i;
#endif

	// Register packetized stream channel
	state.channel.dest.sink = peridot_rpc_server_sink;
	state.channel.number = PERIDOT_RPCSRV_CHANNEL;
	state.channel.packetized = 1;
	state.channel.use_fd = 0;
	peridot_sw_hostbridge_gen2_register_channel(&state.channel);

#ifdef PERIDOT_RPCSRV_MULTI_THREAD
	sem_init(&state.sem, 0, 0);
	for (i = 0; i < (PERIDOT_RPCSRV_WORKER_THREADS); ++i) {
        pthread_t tid;
        int result;
		result = pthread_create(&tid, NULL, peridot_rpc_server_worker, NULL);
        (void)tid;
        if (result != 0) {
            return -result;
        }
	}
#endif

    return 0;
}

/*
 * Register function as RPC-callable
 */
int peridot_rpc_server_register_method(const char *name, peridot_rpc_server_function func)
{
	peridot_rpc_server_method_entry *entry;
	entry = malloc(sizeof(*entry) + strlen(name) + 1);
	if (!entry) {
		return -ENOMEM;
	}
	entry->next = NULL;
	entry->func = func;
	strcpy(entry->name, name);
	if (state.method_last) {
		state.method_last->next = entry;
	} else {
		state.method_first = entry;
	}
    state.method_last = entry;
    return 0;
}

/**
 * @func peridot_rpc_server_service
 * @brief Execute server service
 */
int peridot_rpc_server_service(void)
{
    void *input;
    int off_jsonrpc, off_method, off_params, off_id;
    int result_errno;
    const char *method;
    peridot_rpc_server_method_entry *entry;
    void *result = NULL;
    alt_u8 error_doc[16];
    void *output;
    int reply_len;

#ifdef PERIDOT_RPCSRV_MULTI_THREAD
    sem_wait(&state.sem);
#endif
    rpcsrv_job *job = state.pending_job;
    state.pending_job = NULL;
    if (!job) {
        // No job
        return 0;
    }

    // TODO: support batch request

    input = &job->data;
    result_errno = 0;

    if (bson_get_props(input,
        "jsonrpc", &off_jsonrpc,
        "method", &off_method,
        "params", &off_params,
        "id", &off_id,
        NULL) <= 0) {
        result_errno = JSONRPC_ERR_PARSE_ERROR;
        goto reply;
    }

    if ((strcmp(bson_get_string(input, off_jsonrpc, ""), PERIDOT_RPCSRV_JSONRPC_VER) != 0) ||
        ((method = bson_get_string(input, off_method, NULL)) == NULL)) {
        result_errno = JSONRPC_ERR_INVALID_REQUEST;
        goto reply;
    }

    entry = peridot_rpc_server_find_method(method);
    if (!entry) {
        result_errno = JSONRPC_ERR_METHOD_NOT_FOUND;
        goto reply;
    }

    result = (*entry->func)(bson_get_subdocument(input, off_params, (void *)bson_empty_document));
    if (off_id < 0) {
        // Notification => Do not reply (even if error occurs)
        free(result);
        return 0;
    }
    result_errno = result ? 0 : errno;

reply:
    reply_len = bson_empty_size;
    reply_len += bson_measure_string("jsonrpc", PERIDOT_RPCSRV_JSONRPC_VER);
    reply_len += bson_measure_element("id", input, off_id);
    if (result_errno == 0) {
        // Success
        if (result) {
            reply_len += bson_measure_subdocument("result", result);
        } else {
            reply_len += bson_measure_null("result");
        }
    } else {
        // Fail
        bson_create_empty_document(error_doc);
        bson_set_int32(error_doc, "code", result_errno);
        reply_len += bson_measure_subdocument("error", error_doc);
    }
    output = malloc(reply_len);
    if (!output) {
        if (result_errno != 0) {
            // FIXME: Double error => Ignore this packet
            return 0;
        }
        result_errno = JSONRPC_ERR_INTERNAL_ERROR;
        free(result);
        result = NULL;
        goto reply;
    }

    bson_create_empty_document(output);
    bson_set_string(output, "jsonrpc", PERIDOT_RPCSRV_JSONRPC_VER);
    bson_set_element(output, "id", input, off_id);
    if (result_errno == 0) {
        if (result) {
            bson_set_subdocument(output, "result", result);
            free(result);
        } else {
            bson_set_null(output, "result");
        }
    } else {
        bson_set_subdocument(output, "error", error_doc);
    }
    peridot_sw_hostbridge_gen2_source(&state.channel, output, reply_len, HOSTBRIDGE_GEN2_SOURCE_PACKETIZED);
    free(output);
    return 0;
}
