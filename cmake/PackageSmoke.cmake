# SPDX-License-Identifier: MPL-2.0
if(NOT DEFINED ARCHIVE OR NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "Package archive not found: ${ARCHIVE}")
endif()

get_filename_component(archive_dir "${ARCHIVE}" DIRECTORY)
set(extract_dir "${archive_dir}/smoke")
file(REMOVE_RECURSE "${extract_dir}")
file(MAKE_DIRECTORY "${extract_dir}")
file(ARCHIVE_EXTRACT INPUT "${ARCHIVE}" DESTINATION "${extract_dir}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen "${extract_dir}/bin/hieda" --smoke-test
    RESULT_VARIABLE smoke_result
    OUTPUT_VARIABLE smoke_output
    ERROR_VARIABLE smoke_error
)
if(NOT smoke_result EQUAL 0)
    message(FATAL_ERROR "Packaged Hieda failed to launch:\n${smoke_output}\n${smoke_error}")
endif()
message(STATUS "Packaged Hieda launched successfully")
