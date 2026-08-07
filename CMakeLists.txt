# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.24)

project(Hieda VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LMDB REQUIRED IMPORTED_TARGET "lmdb>=0.9.30")
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 QuickDialogs2)
qt_standard_project_setup(REQUIRES 6.8)

add_library(hieda_notebook
    src/notebook/notebook_session.cpp
)
add_library(Hieda::Notebook ALIAS hieda_notebook)
target_include_directories(hieda_notebook PUBLIC include)
target_link_libraries(hieda_notebook PRIVATE PkgConfig::LMDB)
target_compile_options(hieda_notebook PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

qt_add_executable(hieda
    src/app/main.cpp
    src/app/notebook_controller.cpp
    src/app/notebook_controller.hpp
)
qt_add_qml_module(hieda
    URI Hieda
    VERSION 1.0
    NO_IMPORT_SCAN
    QML_FILES qml/Main.qml
)
target_link_libraries(hieda PRIVATE
    Hieda::Notebook
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
)
target_compile_options(hieda PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

include(GNUInstallDirs)
install(TARGETS hieda RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
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
    target_link_libraries(notebook_session_test PRIVATE Hieda::Notebook Catch2::Catch2WithMain)
    include(Catch)
    catch_discover_tests(notebook_session_test)
    add_test(NAME hieda_app_smoke COMMAND hieda --smoke-test)
    set_tests_properties(hieda_app_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
