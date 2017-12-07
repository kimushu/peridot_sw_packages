#
# digests_sw.tcl
#

create_sw_package digests

set_sw_property version 1.2
set_sw_property auto_initialize true
set_sw_property bsp_subdirectory services

set_sw_property alt_sys_init_priority 100

#
# Source file listings...
#

add_sw_property c_source HAL/src/md5.c
add_sw_property include_source HAL/inc/md5.h

add_sw_property c_source HAL/src/crc32.c
add_sw_property include_source HAL/inc/crc32.h

add_sw_property include_source HAL/inc/digests.h
add_sw_property include_directory inc

add_sw_property supported_bsp_type HAL

#
# BSP settings...
#
add_sw_setting boolean_define_only system_h_define md5.enable DIGESTS_MD5_ENABLE 1 "Enable MD5 digest"
add_sw_setting boolean_define_only system_h_define md5.static_table DIGESTS_MD5_STATIC_TABLE 1 "Use statically defined table for MD5. Turn off to reduce .rodata section size. (However, dynamic table construction requires double-precision math functions.)"

add_sw_setting boolean_define_only system_h_define crc32.enable DIGESTS_CRC32_ENABLE 1 "Enable CRC-32 digest"
add_sw_setting boolean_define_only system_h_define crc32.static_table DIGESTS_CRC32_STATIC_TABLE 1 "Use statically defined table for CRC-32. Turn off to reduce .rodata section size."

# End of file
