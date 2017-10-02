#ifndef __PERIDOT_RPC_SERVER_H__
#define __PERIDOT_RPC_SERVER_H__

#include "alt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PERIDOT_RPCSRV_IF_VERSION	0x0101
#define PERIDOT_RPCSRV_JSONRPC_VER	"2.0"

enum {
	JSONRPC_ERR_PARSE_ERROR      = -32700,
	JSONRPC_ERR_INVALID_REQUEST  = -32600,
	JSONRPC_ERR_METHOD_NOT_FOUND = -32601,
	JSONRPC_ERR_INVALID_PARAMS   = -32602,
	JSONRPC_ERR_INTERNAL_ERROR   = -32603,
};

typedef void *(*peridot_rpc_server_function)(const void *params);

typedef struct peridot_rpc_server_callback_s {
	void (*func)(void);
	struct peridot_rpc_server_callback_s *next;
} peridot_rpc_server_callback;

typedef struct peridot_rpc_server_method_entry_s {
	struct peridot_rpc_server_method_entry_s *next;
	peridot_rpc_server_function func;
	char name[0];
} peridot_rpc_server_method_entry;

extern int peridot_rpc_server_init(void);
extern int peridot_rpc_server_register_method(const char *name, peridot_rpc_server_function func);
extern int peridot_rpc_server_service(void);

#define PERIDOT_RPC_SERVER_INSTANCE(name, state) extern int alt_no_storage
#define PERIDOT_RPC_SERVER_INIT(name, state) \
	peridot_rpc_server_init()

#ifdef __cplusplus
}	/* extern "C" */
#endif

#endif  /* __PERIDOT_RPC_SERVER_H__ */
