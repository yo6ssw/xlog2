cmake_minimum_required(VERSION 3.16)
project(xlog2
    VERSION 0.4.5
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
pkg_check_modules(OPUS   REQUIRED IMPORTED_TARGET opus)   # rig audio stream client (decode)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)  # rig audio playback + sidetone
pkg_check_modules(DBUS   REQUIRED IMPORTED_TARGET dbus-1) # RealtimeKit RT scheduling for the keyer

# multimaster: zero-dependency LAN gossip mesh (git submodule). Powers the
# symmetric, auto-discovering logbook-sync transport. Static lib, links only
# pthreads; we don't want its tests/examples in our build.
set(MULTIMASTER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(MULTIMASTER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/multimaster EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# xlog_core: the toolkit-neutral core. Deliberately NOT linked against gtkmm or
# Qt, so any accidental toolkit include in a core source fails to compile — the
# UI/business-logic boundary is enforced by the build itself.
# ---------------------------------------------------------------------------
add_library(xlog_core STATIC
    src/core/domain/LogBook.cpp
    src/core/domain/Adif.cpp
    src/core/domain/Xlog.cpp
    src/core/domain/Bands.cpp
    src/core/domain/Dxcc.cpp
    src/core/logic/DxccDeriver.cpp
    src/core/logic/CwExpander.cpp
    src/core/logic/Statistics.cpp
    src/core/logic/DupeMessage.cpp
    src/core/logic/QsoMapper.cpp
    src/core/logic/Geo.cpp
    src/core/logic/Uuid.cpp
    src/core/settings/IniFile.cpp
    src/core/settings/Settings.cpp
    src/core/presenter/LogPagePresenter.cpp
    src/core/presenter/MainPresenter.cpp
    src/core/presenter/SyncCoordinator.cpp
    src/core/presenter/QrzPeer.cpp
    src/core/platform/ProcessRunner.cpp
    src/core/platform/Rtkit.cpp
    src/core/services/Rig.cpp
    src/core/services/Udp.cpp
    src/core/services/Lotw.cpp
    src/core/services/Qrz.cpp
    src/core/services/QrzCache.cpp
    src/core/services/CwKeyer.cpp
    src/core/services/DxCluster.cpp
    src/core/services/SyncProtocol.cpp
    src/core/services/LogbookSync.cpp
    src/core/services/Audio.cpp
    src/core/services/CwSkimmer.cpp
    src/core/services/RemotePaddleKeyer.cpp
    src/core/services/HidPaddleInput.cpp
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
    PkgConfig::OPUS
    PkgConfig::PIPEWIRE
    PkgConfig::DBUS
    Threads::Threads)
# mesh transport used only inside LogbookSync.cpp (private); a static lib still
# propagates this to the linked frontends.
target_link_libraries(xlog_core PRIVATE multimaster::multimaster)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(xlog_core PRIVATE -Wall -Wextra)
endif()

# Tell the core where shared data (coastline, etc.) gets installed, so the
# runtime search honours the actual CMAKE_INSTALL_PREFIX (e.g. /usr vs the
# default /usr/local) instead of guessing.
target_compile_definitions(xlog_core PRIVATE
    XLOG_INSTALL_DATADIR="${CMAKE_INSTALL_PREFIX}/share/xlog2")

# Bake the project version (single source of truth) into the build; PUBLIC so it
# reaches the frontends that include Version.h for the title bar / About dialog.
target_compile_definitions(xlog_core PUBLIC
    XLOG_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# xlog2-audioplay: headless tool that reads the xlog2 config and plays the cwsd
# rig-audio stream. Toolkit-neutral: links only xlog_core (no GTK/Qt).
# ---------------------------------------------------------------------------
add_executable(xlog2-audioplay src/tools/AudioPlay.cpp)
target_link_libraries(xlog2-audioplay PRIVATE xlog_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(xlog2-audioplay PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# xlog2-syncd: headless backup peer — replicates the synced logbook and enriches
# the shared qrz.com cache. Toolkit-neutral: links only xlog_core (no GTK/Qt).
# ---------------------------------------------------------------------------
add_executable(xlog2-syncd src/tools/SyncDaemon.cpp)
target_link_libraries(xlog2-syncd PRIVATE xlog_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(xlog2-syncd PRIVATE -Wall -Wextra)
endif()
install(TARGETS xlog2-syncd RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# gtkmm backend
# ---------------------------------------------------------------------------
if(XLOG_BUILD_GTK)
    pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)

    add_executable(xlog2-gtk
        src/main.cpp
        src/XlogApplication.cpp
        src/MainWindow.cpp
        src/SettingsDialog.cpp
        src/LogPage.cpp
        src/DxClusterPanel.cpp
        src/RigPanel.cpp
        src/CwSkimmerPanel.cpp
        src/MapPanel.cpp
    )
    target_link_libraries(xlog2-gtk PRIVATE xlog_core PkgConfig::GTKMM)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(xlog2-gtk PRIVATE -Wall -Wextra)
    endif()
    install(TARGETS xlog2-gtk RUNTIME DESTINATION bin)
    install(FILES packaging/xlog2-gtk.desktop DESTINATION share/applications)
    install(FILES packaging/xlog2-gtk.svg
            DESTINATION share/icons/hicolor/scalable/apps)
endif()

# Shared runtime data (world-map coastline), installed for both frontends.
install(FILES data/coastline.txt DESTINATION share/xlog2)

# udev rule granting the local user access to the USB HID CW paddle (hidraw).
# Lives under /usr/lib (not the multiarch libdir) per udev convention.
install(FILES packaging/99-xlog2-paddle.rules DESTINATION lib/udev/rules.d)

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
        src/qt/QtRigPanel.cpp
        src/qt/QtCwSkimmerPanel.cpp
        src/qt/QtSettingsDialog.cpp
        src/qt/QtMapPanel.cpp
        src/qt/FlowLayout.cpp
        src/qt/QtDispatcher.h     # header-only Q_OBJECT types, listed for AUTOMOC
        src/qt/QsoTableModel.h
        src/qt/QtLogPage.h
        src/qt/QtMainWindow.h
        src/qt/QtDxClusterPanel.h
        src/qt/QtRigPanel.h
        src/qt/QtCwSkimmerPanel.h
        src/qt/QtSettingsDialog.h
        src/qt/QtMapPanel.h
    )
    set_target_properties(xlog2-qt PROPERTIES AUTOMOC ON)
    target_include_directories(xlog2-qt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/qt)
    target_link_libraries(xlog2-qt PRIVATE xlog_core Qt6::Widgets)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(xlog2-qt PRIVATE -Wall -Wextra)
    endif()
    install(TARGETS xlog2-qt RUNTIME DESTINATION bin)
    install(FILES packaging/xlog2-qt.desktop DESTINATION share/applications)
    install(FILES packaging/xlog2-qt.svg
            DESTINATION share/icons/hicolor/scalable/apps)

    # -----------------------------------------------------------------------
    # xlog2-paddle: standalone iambic-paddle practice keyer. Reuses the core
    # RemotePaddleKeyer in local-only mode (sidetone, no remote keying).
    # -----------------------------------------------------------------------
    add_executable(xlog2-paddle
        src/tools/PaddleKeyer.cpp
        src/qt/QtDispatcher.h     # header-only Q_OBJECT type, listed for AUTOMOC
    )
    set_target_properties(xlog2-paddle PROPERTIES AUTOMOC ON)
    target_include_directories(xlog2-paddle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/qt)
    target_link_libraries(xlog2-paddle PRIVATE xlog_core Qt6::Widgets)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(xlog2-paddle PRIVATE -Wall -Wextra)
    endif()
    install(TARGETS xlog2-paddle RUNTIME DESTINATION bin)
    install(FILES packaging/xlog2-paddle.desktop DESTINATION share/applications)
    install(FILES packaging/xlog2-paddle.svg
            DESTINATION share/icons/hicolor/scalable/apps)
endif()
