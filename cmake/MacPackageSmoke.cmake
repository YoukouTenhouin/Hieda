# SPDX-License-Identifier: MPL-2.0
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "DMG smoke testing requires a macOS host")
endif()
if(NOT DEFINED DMG OR NOT EXISTS "${DMG}")
    message(FATAL_ERROR "DMG not found: ${DMG}")
endif()

get_filename_component(dmg_dir "${DMG}" DIRECTORY)
set(mount_point "${dmg_dir}/mounted-dmg")
file(REMOVE_RECURSE "${mount_point}")
file(MAKE_DIRECTORY "${mount_point}")

execute_process(
    COMMAND hdiutil attach "${DMG}" -nobrowse -readonly -mountpoint "${mount_point}"
    RESULT_VARIABLE attach_result
    OUTPUT_QUIET
)
if(NOT attach_result EQUAL 0)
    message(FATAL_ERROR "Failed to mount ${DMG}")
endif()

execute_process(
    COMMAND "${mount_point}/Hieda.app/Contents/MacOS/Hieda" --smoke-test
    RESULT_VARIABLE smoke_result
    OUTPUT_VARIABLE smoke_output
    ERROR_VARIABLE smoke_error
)
execute_process(COMMAND hdiutil detach "${mount_point}" RESULT_VARIABLE detach_result OUTPUT_QUIET)

if(NOT smoke_result EQUAL 0)
    message(FATAL_ERROR "Packaged Hieda failed to launch:\n${smoke_output}\n${smoke_error}")
endif()
if(NOT detach_result EQUAL 0)
    message(FATAL_ERROR "Hieda launched, but the test DMG could not be detached")
endif()
message(STATUS "Packaged Hieda launched successfully")
