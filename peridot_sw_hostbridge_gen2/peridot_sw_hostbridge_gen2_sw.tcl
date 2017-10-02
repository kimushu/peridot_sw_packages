#
# peridot_sw_hostbridge_gen2_sw.tcl
#

create_sw_package peridot_sw_hostbridge_gen2

set_sw_property version 16.1
set_sw_property auto_initialize true
set_sw_property bsp_subdirectory services

#
# Source file listings...
#

add_sw_property c_source HAL/src/peridot_sw_hostbridge_gen2.c
add_sw_property c_source HAL/src/peridot_sw_hostbridge_gen2_avm.c

add_sw_property include_source HAL/inc/peridot_sw_hostbridge_gen2.h
add_sw_property include_directory inc

add_sw_property supported_bsp_type HAL

# End of file
