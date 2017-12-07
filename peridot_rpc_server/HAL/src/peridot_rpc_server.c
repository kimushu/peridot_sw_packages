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
    peridot_rpc_server_async_context context;
    struct rpcsrv_job_s *next;
    union {
        alt_u8 bytes[0];
        alt_u32 words[0];
    } data;
} rpcsrv_job;

struct peridot_rpc_server_state_s {
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
    pthread_t tids[PERIDOT_RPCSRV_WORKER_THREADS];
#endif
    peridot_rpc_server_method_entry *method_first, *method_last;
} peridot_rpc_server_state __attribute__((weak));

static struct peridot_rpc_server_state_s state
__attribute__((alias("peridot_rpc_server_state")));

static int send_reply(rpcsrv_job *job, int off_id, void *result, int result_errno);

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
            state.eop_prefix = 0;
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
    char name[16];
#endif

    // Register packetized stream channel
    state.channel.dest.sink = peridot_rpc_server_sink;
    state.channel.number = PERIDOT_RPCSRV_CHANNEL;
    state.channel.packetized = 1;
    state.channel.use_fd = 0;
    peridot_sw_hostbridge_gen2_register_channel(&state.channel);

#ifdef PERIDOT_RPCSRV_MULTI_THREAD
    sem_init(&state.sem, 0, 0);
    strcpy(name, "rpc_server_x");
    for (i = 0; i < (PERIDOT_RPCSRV_WORKER_THREADS); ++i) {
        int result;
        result = pthread_create(&state.tids[i], NULL, peridot_rpc_server_worker, NULL);
        if (result != 0) {
            return -result;
        }
        name[11] = i + '0';
        pthread_setname_np(state.tids[i], name);
    }
#endif

    return 0;
}

/*
 * Register sync function as RPC-callable
 */
static int register_method(const char *name, peridot_rpc_server_sync_function sync, peridot_rpc_server_async_function async)
{
    peridot_rpc_server_method_entry *entry;
    entry = malloc(sizeof(*entry) + strlen(name) + 1);
    if (!entry) {
        return -ENOMEM;
    }
    entry->next = NULL;
    entry->sync = sync;
    entry->async = async;
    strcpy(entry->name, name);
    if (state.method_last) {
        state.method_last->next = entry;
    } else {
        state.method_first = entry;
    }
    state.method_last = entry;
    return 0;
}

/*
 * Register sync function as RPC-callable
 */
int peridot_rpc_server_register_sync_method(const char *name, peridot_rpc_server_sync_function func)
{
    return register_method(name, func, NULL);
}

/*
 * Register async function as RPC-callable
 */
int peridot_rpc_server_register_async_method(const char *name, peridot_rpc_server_async_function func)
{
    return register_method(name, NULL, func);
}

/**
 * @func async_reply
 * @brief Send reply for async methods
 */
static int async_reply(peridot_rpc_server_async_context *context, void *result_or_error, int result_errno)
{
    rpcsrv_job *job = (rpcsrv_job *)context;
    int off_id;

    if (bson_get_props(&job->data, "id", &off_id) == 1) {
        return send_reply(job, off_id, result_or_error, result_errno);
    }

    // For notify call
    free(result_or_error);
    free(job);
    return 0;
}

/**
 * @func peridot_rpc_server_async_callback
 * @brief Callback for async methods
 */
int peridot_rpc_server_async_callback(peridot_rpc_server_async_context *context, void *result, int result_errno)
{
    return async_reply(context, result_errno == 0 ? result : NULL, result_errno);
}

/**
 * @func peridot_rpc_server_async_callback_error
 * @brief Callback with error for async methods
 */
int peridot_rpc_server_async_callback_error(peridot_rpc_server_async_context *context, void *error)
{
    return async_reply(context, error, JSONRPC_ERR_INTERNAL_ERROR);
}

/**
 * @func peridot_rpc_server_service
 * @brief Execute server service
 */
int peridot_rpc_server_service(void)
{
    void *input;
    int off_jsonrpc, off_method, off_params, off_id;
    const char *method;
    peridot_rpc_server_method_entry *entry;
    void *result;

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
    result = NULL;

    if (bson_get_props(input,
        "jsonrpc", &off_jsonrpc,
        "method", &off_method,
        "params", &off_params,
        "id", &off_id,
        NULL) <= 0) {
        errno = JSONRPC_ERR_PARSE_ERROR;
        goto reply;
    }

    if ((strcmp(bson_get_string(input, off_jsonrpc, ""), PERIDOT_RPCSRV_JSONRPC_VER) != 0) ||
        ((method = bson_get_string(input, off_method, NULL)) == NULL)) {
        errno = JSONRPC_ERR_INVALID_REQUEST;
        goto reply;
    }

    entry = peridot_rpc_server_find_method(method);
    if (entry) {
        const void *params = bson_get_subdocument(input, off_params, NULL);
        if (entry->sync) {
            // Synchronous call
            errno = 0;
            result = (*entry->sync)(params);
        } else {
            // Asynchronous call
            job->context.params = params;
            errno = (*entry->async)(&job->context);
            if (errno == 0) {
                // Successfully queued
                // Job will free by async_callback
                return 0;
            }
        }
    } else {
        errno = JSONRPC_ERR_METHOD_NOT_FOUND;
    }

    if (off_id < 0) {
        // Notification => Do not reply (even if error occurs)
        free(result);
        free(job);
        return 0;
    }

reply:
    return send_reply(job, off_id, result, result ? 0 : errno);
}

/**
 * @func send_reply
 * @brief Send reply message and cleanup job
 */
static int send_reply(rpcsrv_job *job, int off_id, void *result_or_error, int result_errno)
{
    alt_u8 error_doc_buffer[16];
    void *error_doc;
    void *output;
    int reply_len;
    void *input = &job->data;

reply:
    reply_len = bson_empty_size;
    reply_len += bson_measure_string("jsonrpc", PERIDOT_RPCSRV_JSONRPC_VER);
    reply_len += bson_measure_element("id", input, off_id);
    if (result_errno == 0) {
        // Success
        if (result_or_error) {
            reply_len += bson_measure_subdocument("result", result_or_error);
        } else {
            reply_len += bson_measure_null("result");
        }
    } else {
        // Fail
        if (result_or_error) {
            error_doc = result_or_error;
        } else {
            bson_create_empty_document(error_doc_buffer);
            bson_set_int32(error_doc_buffer, "code", result_errno);
            error_doc = error_doc_buffer;
        }
        reply_len += bson_measure_subdocument("error", error_doc);
    }
    output = malloc(reply_len);
    if (!output) {
        if (result_errno != 0) {
            // FIXME: Double error => Ignore this packet
            free(result_or_error);
            free(job);
            return 0;
        }
        result_errno = JSONRPC_ERR_INTERNAL_ERROR;
        free(result_or_error);
        result_or_error = NULL;
        goto reply;
    }

    bson_create_empty_document(output);
    bson_set_string(output, "jsonrpc", PERIDOT_RPCSRV_JSONRPC_VER);
    bson_set_element(output, "id", input, off_id);
    free(job);

    if (result_errno == 0) {
        if (result) {
            bson_set_subdocument(output, "result", result_or_error);
            free(result_or_error);
        } else {
            bson_set_null(output, "result");
        }
    } else {
        bson_set_subdocument(output, "error", error_doc);
        free(result_or_error);
    }
    peridot_sw_hostbridge_gen2_source(&state.channel, output, reply_len, HOSTBRIDGE_GEN2_SOURCE_PACKETIZED);
    free(output);
    return 0;
}
