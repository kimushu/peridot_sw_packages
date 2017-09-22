#
# named_fifo_sw.tcl
#

create_sw_package named_fifo

set_sw_property version 16.1
set_sw_property auto_initialize true
set_sw_property bsp_subdirectory services

#
# Source file listings...
#

add_sw_property c_source HAL/src/named_fifo.c

add_sw_property include_source HAL/inc/named_fifo.h
add_sw_property include_directory inc

add_sw_property supported_bsp_type HAL

#
# BSP settings...
#
add_sw_setting boolean system_h_define stdin.enable NAMED_FIFO_STDIN_ENABLE 0 "Enable named FIFO for standard input. When enabled, hal.stdin in Main page must be set to 'none'."
add_sw_setting quoted_string system_h_define stdin.name NAMED_FIFO_STDIN_NAME "/dev/stdin" "Name of stdin device"
add_sw_setting decimal_number system_h_define stdin.size NAMED_FIFO_STDIN_SIZE 1024 "Buffer length for stdin device (in bytes)"

add_sw_setting boolean system_h_define stdout.enable NAMED_FIFO_STDOUT_ENABLE 0 "Enable named FIFO for standard output. When enabled, hal.stdout in Main page must be set to 'none'."
add_sw_setting quoted_string system_h_define stdout.name NAMED_FIFO_STDOUT_NAME "/dev/stdout" "Name of stdout device"
add_sw_setting decimal_number system_h_define stdout.size NAMED_FIFO_STDOUT_SIZE 1024 "Buffer length for stdout device (in bytes)"

add_sw_setting boolean system_h_define stderr.enable NAMED_FIFO_STDERR_ENABLE 0 "Enable named FIFO for standard error. When enabled, hal.stderr in Main page must be set to 'none'."
add_sw_setting quoted_string system_h_define stderr.name NAMED_FIFO_STDERR_NAME "/dev/stderr" "Name of stderr device"
add_sw_setting decimal_number system_h_define stderr.size NAMED_FIFO_STDERR_SIZE 1024 "Buffer length for stderr device (in bytes)"

add_sw_setting boolean system_h_define stdio.initially_opened NAMED_FIFO_STDIO_INIT_OPENED 1 "Start system with stdio opened."

# End of file
