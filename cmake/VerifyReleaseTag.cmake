# SPDX-License-Identifier: MPL-2.0
if(NOT DEFINED SOURCE_DIR OR NOT DEFINED RELEASE_TAG)
    message(FATAL_ERROR "SOURCE_DIR and RELEASE_TAG are required")
endif()

file(READ "${SOURCE_DIR}/CMakeLists.txt" project_file)
string(REGEX MATCH "project\\(Hieda VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)" _ "${project_file}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "Could not read the Hieda project version")
endif()
if(NOT RELEASE_TAG STREQUAL "v${CMAKE_MATCH_1}")
    message(FATAL_ERROR "Release tag ${RELEASE_TAG} does not match project version v${CMAKE_MATCH_1}")
endif()
message(STATUS "Release tag ${RELEASE_TAG} matches the project version")
