#
# peridot_rpc_server_sw.tcl
#

create_sw_package peridot_rpc_server

set_sw_property version 1.6
set_sw_property auto_initialize true
set_sw_property bsp_subdirectory services

# This module should be initialized after peridot_swi driver
set_sw_property alt_sys_init_priority 2000

#
# Source file listings...
#

add_sw_property c_source HAL/src/peridot_rpc_server.c
add_sw_property c_source HAL/src/bson.c

add_sw_property include_source HAL/inc/peridot_rpc_server.h
add_sw_property include_source HAL/inc/bson.h
add_sw_property include_directory inc

add_sw_property supported_bsp_type HAL

#
# BSP settings...
#
add_sw_setting decimal_number system_h_define channel PERIDOT_RPCSRV_CHANNEL 1 "Channel number for RPC server"
add_sw_setting decimal_number system_h_define request_length PERIDOT_RPCSRV_MAX_REQUEST_LENGTH 8192 "Maximum byte length of request data"
add_sw_setting boolean_define_only system_h_define multi_threaded PERIDOT_RPCSRV_MULTI_THREADED 0 "Use RPC server in multi-threaded environment"
add_sw_setting decimal_number system_h_define worker_threads PERIDOT_RPCSRV_WORKER_THREADS 0 "Number of worker threads for processing requests. When non-zero value is specified, multi-threading (pthread) support is required."

# End of file
