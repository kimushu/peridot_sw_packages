#
# rubic_agent_sw.tcl
#

create_sw_package rubic_agent

set_sw_property version 16.1
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
add_sw_setting quoted_string system_h_define root.name RUBIC_AGENT_ROOT_NAME "/sys/rubic" "Root name of Rubic agent"
add_sw_setting quoted_string system_h_define rubic.version RUBIC_AGENT_RUBIC_VERSION ">=0.99.1" "Supported Rubic version (semver range syntax)"
add_sw_setting quoted_string system_h_define runtime1.name RUBIC_AGENT_RUNTIME1_NAME "" "Name of runtime #1"
add_sw_setting quoted_string system_h_define runtime1.version RUBIC_AGENT_RUNTIME1_VERSION "0.0.1" "Version of runtime #1"
add_sw_setting boolean_define_only system_h_define runtime2.present RUBIC_AGENT_RUNTIME2_PRESENT 0 "Set if this system has second runtime (runtime #2)"
add_sw_setting quoted_string system_h_define runtime2.name RUBIC_AGENT_RUNTIME2_NAME "" "Name of runtime #2"
add_sw_setting quoted_string system_h_define runtime2.version RUBIC_AGENT_RUNTIME2_VERSION "0.0.1" "Version of runtime #2 (semver syntax)"
add_sw_setting boolean_define_only system_h_define runtime3.present RUBIC_AGENT_RUNTIME3_PRESENT 0 "Set if this system has third runtime (runtime #3)"
add_sw_setting quoted_string system_h_define runtime3.name RUBIC_AGENT_RUNTIME3_NAME "" "Name of runtime #3"
add_sw_setting quoted_string system_h_define runtime3.version RUBIC_AGENT_RUNTIME3_VERSION "0.0.1" "Version of runtime #3 (semver syntax)"

# End of file
