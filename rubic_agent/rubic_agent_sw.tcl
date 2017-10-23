#
# rubic_agent_sw.tcl
#

create_sw_package rubic_agent

set_sw_property version 1.2
set_sw_property auto_initialize true
set_sw_property bsp_subdirectory services

#
# Source file listings...
#

add_sw_property c_source HAL/src/rubic_agent.c

add_sw_property include_source HAL/inc/rubic_agent.h
add_sw_property include_directory inc

add_sw_property supported_bsp_type TINYTH

#
# BSP settings...
#
add_sw_setting quoted_string system_h_define rubic_version RUBIC_AGENT_RUBIC_VERSION ">=1.0.0" "Supported Rubic version (semver range syntax)"
add_sw_setting decimal_number system_h_define workers_max RUBIC_AGENT_WORKER_THREADS 1 "Maximum number of workers"
add_sw_setting decimal_number system_h_define runtimes_max RUBIC_AGENT_MAX_RUNTIMES 1 "Maximum number of runtimes"
add_sw_setting decimal_number system_h_define storages_max RUBIC_AGENT_MAX_STORAGES 1 "Maximum number of storages"
add_sw_setting boolean_define_only system_h_define enable_programmer RUBIC_AGENT_ENABLE_PROGRAMMER 1 "Enable programmer (Firmware updater)"

# End of file
