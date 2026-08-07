# SPDX-License-Identifier: MPL-2.0
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "AppImage packaging requires a Linux host")
endif()
foreach(required IN ITEMS BUILD_DIR OUTPUT_DIR PROJECT_VERSION SOURCE_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
if(NOT EXISTS "${BUILD_DIR}/cmake_install.cmake")
    message(FATAL_ERROR "Configured build directory not found: ${BUILD_DIR}")
endif()

get_filename_component(build_dir "${BUILD_DIR}" ABSOLUTE)
get_filename_component(output_dir "${OUTPUT_DIR}" ABSOLUTE)
get_filename_component(source_dir "${SOURCE_DIR}" ABSOLUTE)
set(app_dir "${output_dir}/Hieda.AppDir")
set(tool_dir "${output_dir}/tools")
set(linuxdeploy "${tool_dir}/linuxdeploy-x86_64.AppImage")
set(qt_plugin "${tool_dir}/linuxdeploy-plugin-qt")
set(appimage_runtime "${tool_dir}/runtime-x86_64")
set(package_name "hieda-${PROJECT_VERSION}-linux-x86_64.AppImage")

file(REMOVE_RECURSE "${app_dir}")
file(MAKE_DIRECTORY "${output_dir}" "${tool_dir}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${build_dir}" --config Release --prefix "${app_dir}/usr"
    RESULT_VARIABLE install_result
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "Failed to install Hieda into the AppDir")
endif()

function(download_verified url destination expected_hash)
    set(download_tool TRUE)
    if(EXISTS "${destination}")
        file(SHA256 "${destination}" actual_hash)
        if(actual_hash STREQUAL expected_hash)
            set(download_tool FALSE)
        endif()
    endif()
    if(download_tool)
        file(DOWNLOAD "${url}" "${destination}"
            EXPECTED_HASH "SHA256=${expected_hash}"
            TLS_VERIFY ON
            SHOW_PROGRESS
        )
    endif()
endfunction()

download_verified(
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage"
    "${linuxdeploy}"
    "c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
)
download_verified(
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage"
    "${qt_plugin}"
    "15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"
)
download_verified(
    "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64"
    "${appimage_runtime}"
    "1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf"
)
file(CHMOD "${linuxdeploy}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
file(CHMOD "${qt_plugin}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

if(NOT DEFINED QMAKE)
    find_program(QMAKE NAMES qmake6 qmake REQUIRED)
endif()
get_filename_component(qmake "${QMAKE}" ABSOLUTE)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "OUTPUT=${package_name}"
        "VERSION=${PROJECT_VERSION}"
        "ARCH=x86_64"
        "APPIMAGE_EXTRACT_AND_RUN=1"
        "PATH=${tool_dir}:$ENV{PATH}"
        "QMAKE=${qmake}"
        "QML_SOURCES_PATHS=${source_dir}/qml"
        "EXTRA_PLATFORM_PLUGINS=libqoffscreen.so"
        "LDAI_RUNTIME_FILE=${appimage_runtime}"
        "${linuxdeploy}" --appdir "${app_dir}" --plugin qt --output appimage
    WORKING_DIRECTORY "${output_dir}"
    RESULT_VARIABLE package_result
)
if(NOT package_result EQUAL 0 OR NOT EXISTS "${output_dir}/${package_name}")
    message(FATAL_ERROR "Failed to create AppImage: ${output_dir}/${package_name}")
endif()
message(STATUS "Created ${output_dir}/${package_name}")
