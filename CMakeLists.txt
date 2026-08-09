# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.24)

project(Hieda VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Windows|Darwin)$")
    message(FATAL_ERROR "Hieda supports Linux, Windows, and macOS")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Hieda requires a 64-bit process")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
    message(FATAL_ERROR "Hieda requires GCC 11 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "Hieda requires Clang 16 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
    message(FATAL_ERROR "Hieda requires AppleClang 15 or newer")
elseif(MSVC AND MSVC_VERSION LESS 1930)
    message(FATAL_ERROR "Hieda requires MSVC 2022 or newer")
elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang|MSVC)$")
    message(FATAL_ERROR "Hieda supports GCC, Clang, AppleClang, and MSVC")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(unofficial-lmdb 0.9.30 CONFIG QUIET)
if(TARGET unofficial::lmdb::lmdb)
    set(HIEDA_LMDB_TARGET unofficial::lmdb::lmdb)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LMDB REQUIRED IMPORTED_TARGET "lmdb>=0.9.30")
    set(HIEDA_LMDB_TARGET PkgConfig::LMDB)
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 QuickDialogs2 Widgets)
qt_standard_project_setup(REQUIRES 6.8)
include(CTest)

if(WIN32)
    set(HIEDA_PLATFORM_FILE_SOURCE src/notebook/platform_file_windows.cpp)
else()
    set(HIEDA_PLATFORM_FILE_SOURCE src/notebook/platform_file_posix.cpp)
endif()

function(hieda_enable_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4;/permissive-;/Zc:__cplusplus>
    )
endfunction()

add_library(hieda_notebook
    src/notebook/authored_text_parser.cpp
    src/notebook/authored_text_parser.hpp
    src/notebook/notebook_session.cpp
    src/notebook/platform_file.hpp
    ${HIEDA_PLATFORM_FILE_SOURCE}
)
add_library(Hieda::Notebook ALIAS hieda_notebook)
target_include_directories(hieda_notebook PUBLIC include)
target_link_libraries(hieda_notebook PRIVATE ${HIEDA_LMDB_TARGET})
if(BUILD_TESTING)
    target_compile_definitions(hieda_notebook PRIVATE HIEDA_TESTING)
endif()
hieda_enable_warnings(hieda_notebook)

add_library(hieda_qt_adapter
    src/app/notebook_controller.cpp
    src/app/notebook_controller.hpp
)
target_include_directories(hieda_qt_adapter PUBLIC src/app)
target_link_libraries(hieda_qt_adapter PUBLIC Hieda::Notebook Qt6::Core PRIVATE Qt6::Gui)

qt_add_executable(hieda src/app/main.cpp)
if(WIN32)
    target_sources(hieda PRIVATE assets/hieda.rc)
    target_include_directories(hieda PRIVATE assets)
    set_target_properties(hieda PROPERTIES
        OUTPUT_NAME Hieda
        WIN32_EXECUTABLE TRUE
    )
elseif(APPLE)
    set(mac_icon "${CMAKE_CURRENT_BINARY_DIR}/hieda.icns")
    add_custom_command(
        OUTPUT "${mac_icon}"
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png"
            "-DOUTPUT=${mac_icon}"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/mac-icon"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateMacIcon.cmake"
        DEPENDS assets/icon.png cmake/GenerateMacIcon.cmake
        VERBATIM
    )
    target_sources(hieda PRIVATE
        "${mac_icon}"
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    )
    set_source_files_properties("${mac_icon}" PROPERTIES
        GENERATED TRUE
        MACOSX_PACKAGE_LOCATION Resources
    )
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources/licenses"
    )
    set_target_properties(hieda PROPERTIES
        OUTPUT_NAME Hieda
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME Hieda
        MACOSX_BUNDLE_GUI_IDENTIFIER io.github.youkoutenhouin.hieda
        MACOSX_BUNDLE_ICON_FILE hieda.icns
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    )
endif()
set(hieda_qml_options)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND hieda_qml_options NO_IMPORT_SCAN)
endif()
qt_add_qml_module(hieda
    URI Hieda
    VERSION 1.0
    ${hieda_qml_options}
    QML_FILES qml/Main.qml
)
target_link_libraries(hieda PRIVATE
    hieda_qt_adapter
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
    Qt6::Widgets
)
hieda_enable_warnings(hieda)

include(GNUInstallDirs)
install(TARGETS hieda
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
if(NOT APPLE)
    install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/hieda)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(FILES assets/hieda.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES assets/icon.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/1024x1024/apps
        RENAME hieda.png
    )
endif()

if(WIN32 OR APPLE)
    qt_generate_deploy_qml_app_script(
        TARGET hieda
        OUTPUT_SCRIPT hieda_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${hieda_deploy_script})
endif()

set(CPACK_PACKAGE_NAME "hieda")
set(CPACK_PACKAGE_VENDOR "Hieda contributors")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local-first notebook")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_FILE_NAME "hieda-${PROJECT_VERSION}-windows-x86_64")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    include(CPack)
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_PACKAGE_FILE_NAME "hieda-${PROJECT_VERSION}-macos-arm64")
    set(CPACK_DMG_VOLUME_NAME "Hieda ${PROJECT_VERSION}")
    include(CPack)
endif()

if(BUILD_TESTING)
    find_package(Catch2 3 REQUIRED)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    add_executable(notebook_session_test tests/notebook_session_test.cpp)
    target_include_directories(notebook_session_test PRIVATE src/notebook)
    target_link_libraries(notebook_session_test PRIVATE Hieda::Notebook Catch2::Catch2WithMain ${HIEDA_LMDB_TARGET})
    include(Catch)
    set(hieda_catch_skip_arguments)
    if(Catch2_VERSION VERSION_LESS 3.8)
        set(hieda_catch_skip_arguments PROPERTIES SKIP_RETURN_CODE 4)
    endif()
    catch_discover_tests(notebook_session_test ${hieda_catch_skip_arguments})
    add_executable(notebook_controller_test tests/notebook_controller_test.cpp)
    target_link_libraries(notebook_controller_test PRIVATE hieda_qt_adapter Catch2::Catch2WithMain)
    catch_discover_tests(notebook_controller_test)
    add_test(NAME hieda_app_smoke COMMAND hieda --smoke-test)
    add_executable(journal_editor_ui_test tests/journal_editor_ui_test.cpp)
    target_compile_definitions(journal_editor_ui_test
        PRIVATE HIEDA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(journal_editor_ui_test
        PRIVATE hieda_qt_adapter Catch2::Catch2 Qt6::Qml Qt6::Quick Qt6::Test Qt6::Widgets
    )
    add_test(NAME journal_editor_ui_test COMMAND journal_editor_ui_test)
    if(NOT APPLE)
        set_tests_properties(hieda_app_smoke journal_editor_ui_test
            PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    endif()
endif()
