#
# peridot_rpc_server_sw.tcl
#

create_sw_package peridot_rpc_server

set_sw_property version 16.1
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
add_sw_setting decimal_number system_h_define request_length PERIDOT_RPCSRV_REQUEST_LENGTH 1024 "Byte length of request data"
add_sw_setting decimal_number system_h_define response_length PERIDOT_RPCSRV_RESPONSE_LENGTH 1024 "Byte length of response data"
add_sw_setting decimal_number system_h_define worker_threads PERIDOT_RPCSRV_WORKER_THREADS 0 "Number of worker threads for processing requests. When non-zero value is specified, multi-threading (pthread) support is required."
add_sw_setting boolean_define_only system_h_define enable_isolation PERIDOT_RPCSRV_ENABLE_ISOLATION 0 "Enable isolation of shared memory areas by locating them out of .data section."
add_sw_setting quoted_string system_h_define isolated_section PERIDOT_RPCSRV_ISOLATED_SECTION ".public" "Section name to locate shared memory (This is valid only when enable_isolation is true)"

# End of file
