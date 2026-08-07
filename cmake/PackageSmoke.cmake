# SPDX-License-Identifier: MPL-2.0
if(DEFINED APPIMAGE)
    if(NOT EXISTS "${APPIMAGE}")
        message(FATAL_ERROR "AppImage not found: ${APPIMAGE}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env APPIMAGE_EXTRACT_AND_RUN=1 QT_QPA_PLATFORM=offscreen
                "${APPIMAGE}" --smoke-test
        RESULT_VARIABLE smoke_result
        OUTPUT_VARIABLE smoke_output
        ERROR_VARIABLE smoke_error
    )
    if(NOT smoke_result EQUAL 0)
        message(FATAL_ERROR "Packaged Hieda failed to launch:\n${smoke_output}\n${smoke_error}")
    endif()
    message(STATUS "Packaged Hieda launched successfully")
    return()
endif()

if(NOT DEFINED ARCHIVE OR NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "Package archive or AppImage not found")
endif()

get_filename_component(archive_dir "${ARCHIVE}" DIRECTORY)
set(extract_dir "${archive_dir}/smoke")
file(REMOVE_RECURSE "${extract_dir}")
file(MAKE_DIRECTORY "${extract_dir}")
file(ARCHIVE_EXTRACT INPUT "${ARCHIVE}" DESTINATION "${extract_dir}")

if(DEFINED EXECUTABLE_NAME)
    file(GLOB_RECURSE packaged_executables "${extract_dir}/*/${EXECUTABLE_NAME}")
    list(LENGTH packaged_executables executable_count)
    if(NOT executable_count EQUAL 1)
        message(FATAL_ERROR "Expected one ${EXECUTABLE_NAME} in ${ARCHIVE}, found ${executable_count}")
    endif()
    list(GET packaged_executables 0 executable)
else()
    set(executable "${extract_dir}/bin/hieda")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen "${executable}" --smoke-test
    RESULT_VARIABLE smoke_result
    OUTPUT_VARIABLE smoke_output
    ERROR_VARIABLE smoke_error
)
if(NOT smoke_result EQUAL 0)
    message(FATAL_ERROR "Packaged Hieda failed to launch:\n${smoke_output}\n${smoke_error}")
endif()
message(STATUS "Packaged Hieda launched successfully")
