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

/*
//

Canarium経由によるRPC機構(ホスト側がcaller(=クライアント)、PERIDOT側がcallee(=サーバ))

基本的にはjsonrpc(2.0)をBSONエンコーディングしたデータでやりとりする。
バッファはすべてPERIDOT側が用意する必要があるため、サーバ側がリクエスト用バッファや応答データを
サーバ情報として用意し、そのサーバ情報のポインタをSWI.Messageで公開する。
クライアントはそのサーバ情報およびデータをポーリングしてデータをやりとりする。

なお、サーバ側の処理簡略化のため、以下の仕様に制限する。
- jsonrpcのバージョンは文字列の"2.0"に固定する。
- paramsに名前付き引数は使用できない。
- 扱える型は、以下のとおり
  - 0x01 : double
  - 0x02 : NULL終端文字列
  - 0x05 : バイナリデータ
  - 0x10 : 32-bit整数

さらに、PERIDOT側HALの特性にあわせて、以下の仕様を加える
- 戻り値としてresultだけでなく、errnoを返却する(エラー無しの場合はゼロとする)

DはBSONバッファを作成し、reqMaxLengthおよびreqPtrとして公開する。(※)
info {
  reqLen0
  reqPtr0
  reqLen1
  reqPtr1
}
HはjsonrpcのBSONデータを作成し、reqPtrの内部に書き込む(ただし先頭のサイズフィールドは一番最後に)
Dはサイズフィールドが有効(非ゼロ)なら、それを回収してRPCの処理をする。
Dは、reqMaxLengthとreqPtrを新しい値に更新できる。

Dは応答が生成できたら、resPtrとして公開する。(※)
info {
  resLen0
  resPtr0
  resLen1
  resPtr1
}
HはresPtrが有効かつそのサイズフィールドが非ゼロなら、その中身を回収する。
Hはresを回収し終わったら、そのサイズフィールドをゼロにする。
DはresPtrのサイズフィールドがゼロになったら、resPtrを次の応答に書き換える。

※バッファの公開は、PERIDOT側のメモリ使用効率を考慮し、リングバッファに
対応させる。最大2つのパートに分かれていても良いことし、
前半部分のポインタ(非NULL)、前半部分のサイズ(>0)、
後半部分のポインタ(NULL可)、後半部分のサイズ(>=0)の4つのフィールドで表す

*/

typedef struct peridot_rpc_server_buffer_s {
	alt_u32 len;
	void *ptr;
} peridot_rpc_server_buffer;

typedef struct peridot_rpc_server_info_s {
	alt_u16 if_ver;
	alt_u16 reserved;
	alt_u32 host_id[2];
	volatile peridot_rpc_server_buffer request;
	volatile peridot_rpc_server_buffer response;
} peridot_rpc_server_info;

typedef void *(*peridot_rpc_server_function)(const void *params, int max_result_length);

typedef struct peridot_rpc_server_method_entry_s {
	struct peridot_rpc_server_method_entry_s *next;
	peridot_rpc_server_function func;
	char name[0];
} peridot_rpc_server_method_entry;

extern void peridot_rpc_server_init(void);
extern void peridot_rpc_server_register_method(const char *name, peridot_rpc_server_function func);
extern void peridot_rpc_server_process(void);

#define PERIDOT_RPC_SERVER_INSTANCE(name, state) extern int alt_no_storage
#define PERIDOT_RPC_SERVER_INIT(name, state) \
	peridot_rpc_server_init()

#ifdef __cplusplus
}	/* extern "C" */
#endif

#endif  /* __PERIDOT_RPC_SERVER_H__ */
