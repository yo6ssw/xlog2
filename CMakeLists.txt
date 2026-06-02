cmake_minimum_required(VERSION 3.16)
project(xlog2
    VERSION 0.2.0
    DESCRIPTION "A GTK4 amateur-radio logging program (xlog clone)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(XLOG_BUILD_GTK "Build the gtkmm backend" ON)
option(XLOG_BUILD_QT  "Build the Qt Widgets backend" ON)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(HAMLIB REQUIRED IMPORTED_TARGET hamlib)
pkg_check_modules(CURL   REQUIRED IMPORTED_TARGET libcurl)

# ---------------------------------------------------------------------------
# xlog_core: the toolkit-neutral core. Deliberately NOT linked against gtkmm or
# Qt, so any accidental toolkit include in a core source fails to compile — the
# UI/business-logic boundary is enforced by the build itself.
# ---------------------------------------------------------------------------
add_library(xlog_core STATIC
    src/core/domain/LogBook.cpp
    src/core/domain/Adif.cpp
    src/core/domain/Bands.cpp
    src/core/domain/Dxcc.cpp
    src/core/logic/DxccDeriver.cpp
    src/core/logic/CwExpander.cpp
    src/core/logic/Statistics.cpp
    src/core/logic/DupeMessage.cpp
    src/core/logic/QsoMapper.cpp
    src/core/settings/IniFile.cpp
    src/core/settings/Settings.cpp
    src/core/presenter/LogPagePresenter.cpp
    src/core/presenter/MainPresenter.cpp
    src/core/platform/ProcessRunner.cpp
    src/core/services/Rig.cpp
    src/core/services/Udp.cpp
    src/core/services/Lotw.cpp
    src/core/services/Qrz.cpp
    src/core/services/CwKeyer.cpp
    src/core/services/DxCluster.cpp
)
target_include_directories(xlog_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/domain
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/logic
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/settings
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/services
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/view
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/presenter
)
target_link_libraries(xlog_core PUBLIC
    PkgConfig::SQLITE
    PkgConfig::HAMLIB
    PkgConfig::CURL
    Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(xlog_core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# gtkmm backend
# ---------------------------------------------------------------------------
if(XLOG_BUILD_GTK)
    pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)

    add_executable(xlog2-gtk
        src/main.cpp
        src/XlogApplication.cpp
        src/MainWindow.cpp
        src/LogPage.cpp
        src/DxClusterPanel.cpp
    )
    target_link_libraries(xlog2-gtk PRIVATE xlog_core PkgConfig::GTKMM)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(xlog2-gtk PRIVATE -Wall -Wextra)
    endif()
    install(TARGETS xlog2-gtk RUNTIME DESTINATION bin)
    install(FILES packaging/xlog2-gtk.desktop DESTINATION share/applications)
endif()

# ---------------------------------------------------------------------------
# Qt Widgets backend (sources added in a later phase)
# ---------------------------------------------------------------------------
if(XLOG_BUILD_QT)
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(NOT Qt6_FOUND)
        message(WARNING "Qt6 Widgets not found; skipping the Qt backend")
        set(XLOG_BUILD_QT OFF)
    endif()
endif()

if(XLOG_BUILD_QT)
    add_executable(xlog2-qt
        src/qt/main.cpp
        src/qt/QtLogPage.cpp
        src/qt/QtMainWindow.cpp
        src/qt/QtDxClusterPanel.cpp
        src/qt/FlowLayout.cpp
        src/qt/QtDispatcher.h     # header-only Q_OBJECT types, listed for AUTOMOC
        src/qt/QsoTableModel.h
        src/qt/QtLogPage.h
        src/qt/QtMainWindow.h
        src/qt/QtDxClusterPanel.h
    )
    set_target_properties(xlog2-qt PROPERTIES AUTOMOC ON)
    target_include_directories(xlog2-qt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/qt)
    target_link_libraries(xlog2-qt PRIVATE xlog_core Qt6::Widgets)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(xlog2-qt PRIVATE -Wall -Wextra)
    endif()
    install(TARGETS xlog2-qt RUNTIME DESTINATION bin)
    install(FILES packaging/xlog2-qt.desktop DESTINATION share/applications)
endif()
