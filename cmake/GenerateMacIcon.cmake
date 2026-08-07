# SPDX-License-Identifier: MPL-2.0
if(NOT DEFINED SOURCE OR NOT EXISTS "${SOURCE}")
    message(FATAL_ERROR "Icon source not found: ${SOURCE}")
endif()
if(NOT DEFINED OUTPUT OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "OUTPUT and WORK_DIR are required")
endif()

set(iconset "${WORK_DIR}/hieda.iconset")
file(REMOVE_RECURSE "${iconset}")
file(MAKE_DIRECTORY "${iconset}")

foreach(size IN ITEMS 16 32 128 256 512)
    math(EXPR retina_size "${size} * 2")
    execute_process(
        COMMAND sips -z "${size}" "${size}" "${SOURCE}" --out "${iconset}/icon_${size}x${size}.png"
        RESULT_VARIABLE standard_result
        OUTPUT_QUIET
    )
    execute_process(
        COMMAND sips -z "${retina_size}" "${retina_size}" "${SOURCE}" --out "${iconset}/icon_${size}x${size}@2x.png"
        RESULT_VARIABLE retina_result
        OUTPUT_QUIET
    )
    if(NOT standard_result EQUAL 0 OR NOT retina_result EQUAL 0)
        message(FATAL_ERROR "Failed to resize the macOS application icon")
    endif()
endforeach()

execute_process(
    COMMAND iconutil -c icns "${iconset}" -o "${OUTPUT}"
    RESULT_VARIABLE iconutil_result
)
if(NOT iconutil_result EQUAL 0 OR NOT EXISTS "${OUTPUT}")
    message(FATAL_ERROR "Failed to create macOS icon: ${OUTPUT}")
endif()
