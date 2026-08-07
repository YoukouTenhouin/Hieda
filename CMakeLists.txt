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
    src/notebook/notebook_session.cpp
    src/notebook/platform_file.hpp
    ${HIEDA_PLATFORM_FILE_SOURCE}
)
add_library(Hieda::Notebook ALIAS hieda_notebook)
target_include_directories(hieda_notebook PUBLIC include)
target_link_libraries(hieda_notebook PRIVATE ${HIEDA_LMDB_TARGET})
hieda_enable_warnings(hieda_notebook)

add_library(hieda_qt_adapter
    src/app/notebook_controller.cpp
    src/app/notebook_controller.hpp
)
target_include_directories(hieda_qt_adapter PUBLIC src/app)
target_link_libraries(hieda_qt_adapter PUBLIC Hieda::Notebook Qt6::Core)

qt_add_executable(hieda src/app/main.cpp)
if(WIN32)
    set_target_properties(hieda PROPERTIES WIN32_EXECUTABLE TRUE)
elseif(APPLE)
    set_target_properties(hieda PROPERTIES MACOSX_BUNDLE TRUE)
endif()
qt_add_qml_module(hieda
    URI Hieda
    VERSION 1.0
    NO_IMPORT_SCAN
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
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/hieda)

set(CPACK_PACKAGE_NAME "hieda")
set(CPACK_PACKAGE_VENDOR "Hieda contributors")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "TGZ")
set(CPACK_PACKAGE_FILE_NAME "hieda-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)

include(CTest)
if(BUILD_TESTING)
    find_package(Catch2 3 REQUIRED)
    add_executable(notebook_session_test tests/notebook_session_test.cpp)
    target_link_libraries(notebook_session_test PRIVATE Hieda::Notebook Catch2::Catch2WithMain ${HIEDA_LMDB_TARGET})
    include(Catch)
    catch_discover_tests(notebook_session_test)
    add_executable(notebook_controller_test tests/notebook_controller_test.cpp)
    target_link_libraries(notebook_controller_test PRIVATE hieda_qt_adapter Catch2::Catch2WithMain)
    catch_discover_tests(notebook_controller_test)
    add_test(NAME hieda_app_smoke COMMAND hieda --smoke-test)
    set_tests_properties(hieda_app_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
