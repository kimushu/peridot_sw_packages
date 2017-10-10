#
# peridot_client_fs_sw.tcl
#

create_sw_package peridot_client_fs

set_sw_property version 16.1
set_sw_property auto_initialize true
set_sw_property bsp_subdirectory services

# This module should be initialized after peridot_swi driver
set_sw_property alt_sys_init_priority 3000

#
# Source file listings...
#

add_sw_property c_source HAL/src/peridot_client_fs.c

add_sw_property include_source HAL/inc/peridot_client_fs.h
add_sw_property include_directory inc

add_sw_property supported_bsp_type HAL

#
# BSP settings...
#
add_sw_setting decimal_number system_h_define max_fds PERIDOT_CLIENT_FS_MAX_FDS 16 "Maximum number of file descriptors can be opened simultaniously."
add_sw_setting quoted_string system_h_define rw_path_list PERIDOT_CLIENT_FS_RW_PATH "" "A list of one or more full-access files or directories separated by colon(:) characters."
add_sw_setting quoted_string system_h_define ro_path_list PERIDOT_CLIENT_FS_RO_PATH "" "A list of one or more read-only files or directories separated by colon(:) characters."
add_sw_setting quoted_string system_h_define wo_path_list PERIDOT_CLIENT_FS_WO_PATH "" "A list of one or more write-only files or directories separated by colon(:) characters."
add_sw_setting boolean_define_only system_h_define hash_md5 PERIDOT_CLIENT_FS_HASH_MD5 1 "Enable MD5 for hash calculation"
add_sw_setting boolean_define_only system_h_define hash_crc32 PERIDOT_CLIENT_FS_HASH_CRC32 0 "Enable CRC-32 for hash calculation"

# End of file
