# ---------------------------------------------------------------------------
# xlog_mobile — the carved-out, toolkit-neutral sync core for the Android app.
#
# It is the same source tree the desktop frontends use (src/core), reduced to
# exactly the files the log+sync+QRZ+ADIF+DXCC path needs — none of the
# hamlib / opus / pipewire / dbus services. The result links only sqlite3,
# libcurl, libsodium (via multimaster) and pthreads, so it cross-compiles for
# Android while staying byte-for-byte wire-compatible with xlog2/xlog2-syncd.
#
# Two consumers:
#   * the Android app's JNI CMake does add_subdirectory() on this file, then
#     links libxlog2jni against xlog_mobile;
#   * a host build (this file as the top-level project) builds xlog_mobile plus
#     a smoke executable, so the carve-out is validated off-device first.
#
# On Android there is no pkg-config: android/third_party/build-deps.sh produces
# per-ABI static libs under android/third_party/<ABI>/{lib,include} (libsodium,
# and libcurl when XLOG_MOBILE_QRZ=ON) and fetches the sqlite amalgamation into
# android/third_party/sqlite/. We pre-seed multimaster's libsodium search via
# SODIUM_* cache vars *before* its add_subdirectory.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.18)
project(xlog_mobile LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(XLOG_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CORE "${XLOG_ROOT}/src/core")
set(VENDOR "${CMAKE_CURRENT_LIST_DIR}/../third_party")

option(XLOG_MOBILE_QRZ "Build QRZ.com lookups (requires libcurl)" ON)

# --- Android: locate vendored libsodium BEFORE multimaster configures -------
if(ANDROID)
    set(ABIDIR "${VENDOR}/${ANDROID_ABI}")
    if(NOT EXISTS "${ABIDIR}/lib/libsodium.a")
        message(FATAL_ERROR
            "Missing ${ABIDIR}/lib/libsodium.a — run android/third_party/build-deps.sh first")
    endif()
    # multimaster's CMake does find_library(SODIUM_LIBRARY sodium) +
    # find_path(SODIUM_INCLUDE_DIR sodium.h); pre-seed both as cache vars.
    set(SODIUM_LIBRARY     "${ABIDIR}/lib/libsodium.a" CACHE FILEPATH "" FORCE)
    set(SODIUM_INCLUDE_DIR "${ABIDIR}/include"         CACHE PATH     "" FORCE)
    # Crucial: stop multimaster's pkg_check_modules from picking up the *host*
    # libsodium (the host pkg-config is on PATH during a cross build), which would
    # shadow our vendored prebuilt with wrong-arch headers/libs. Disabling
    # PkgConfig forces its find_library/find_path branch, which honours the cache
    # vars above.
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON)
endif()

# --- multimaster mesh (same submodule the desktop links) -------------------
set(MULTIMASTER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(MULTIMASTER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${XLOG_ROOT}/third_party/multimaster
                 ${CMAKE_BINARY_DIR}/multimaster EXCLUDE_FROM_ALL)

# --- the carve-out static library ------------------------------------------
set(XLOG_MOBILE_SRC
    ${CORE}/domain/LogBook.cpp
    ${CORE}/domain/Adif.cpp
    ${CORE}/domain/Bands.cpp
    ${CORE}/domain/Dxcc.cpp
    ${CORE}/domain/Xlog.cpp
    ${CORE}/logic/DxccDeriver.cpp
    ${CORE}/logic/QsoMapper.cpp
    ${CORE}/logic/DupeMessage.cpp
    ${CORE}/logic/CwExpander.cpp
    ${CORE}/logic/Uuid.cpp
    ${CORE}/presenter/LogPagePresenter.cpp
    ${CORE}/presenter/SyncCoordinator.cpp
    ${CORE}/services/SyncProtocol.cpp
    ${CORE}/services/LogbookSync.cpp
)
# QRZ (qrz.com) pulls libcurl — the hardest Android dep. Make it optional so a
# minimal-deps APK can be built without TLS/curl while still doing log+sync.
if(XLOG_MOBILE_QRZ)
    list(APPEND XLOG_MOBILE_SRC
        ${CORE}/presenter/QrzPeer.cpp
        ${CORE}/services/Qrz.cpp
        ${CORE}/services/QrzCache.cpp)
endif()
# On Android, compile the sqlite3 amalgamation straight into the lib.
if(ANDROID)
    list(APPEND XLOG_MOBILE_SRC ${VENDOR}/sqlite/sqlite3.c)
endif()

add_library(xlog_mobile STATIC ${XLOG_MOBILE_SRC})

target_include_directories(xlog_mobile PUBLIC
    ${CORE} ${CORE}/domain ${CORE}/logic ${CORE}/settings
    ${CORE}/platform ${CORE}/services ${CORE}/view ${CORE}/presenter)

target_compile_definitions(xlog_mobile PUBLIC XLOG_VERSION="android")

# --- third-party deps: pkg-config on the host, vendored prebuilts on NDK ----
if(ANDROID)
    target_include_directories(xlog_mobile PUBLIC
        ${ABIDIR}/include ${VENDOR}/sqlite)
    target_link_libraries(xlog_mobile PUBLIC ${ABIDIR}/lib/libsodium.a)
    if(XLOG_MOBILE_QRZ)
        if(NOT EXISTS "${ABIDIR}/lib/libcurl.a")
            message(FATAL_ERROR
                "XLOG_MOBILE_QRZ=ON but ${ABIDIR}/lib/libcurl.a is missing — "
                "build it (build-deps.sh) or configure with -DXLOG_MOBILE_QRZ=OFF")
        endif()
        # libcurl's static deps (TLS + zlib) are vendored alongside it; link any
        # extra .a the build produced. BoringSSL is bundled by build-deps.sh.
        file(GLOB CURL_EXTRA "${ABIDIR}/lib/lib*.a")
        target_link_libraries(xlog_mobile PUBLIC ${ABIDIR}/lib/libcurl.a ${CURL_EXTRA})
    endif()
    # Android Bionic has pthreads built in; no -lpthread.
else()
    find_package(PkgConfig REQUIRED)
    find_package(Threads REQUIRED)
    pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
    target_link_libraries(xlog_mobile PUBLIC PkgConfig::SQLITE Threads::Threads)
    if(XLOG_MOBILE_QRZ)
        pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
        target_link_libraries(xlog_mobile PUBLIC PkgConfig::CURL)
    endif()
endif()

if(NOT XLOG_MOBILE_QRZ)
    target_compile_definitions(xlog_mobile PUBLIC XLOG_NO_QRZ=1)
endif()

# multimaster is used only inside LogbookSync.cpp, but a static lib propagates
# the dependency, so link it PUBLIC.
target_link_libraries(xlog_mobile PUBLIC multimaster::multimaster)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(xlog_mobile PRIVATE -Wall -Wextra)
endif()

# --- host-only smoke executable --------------------------------------------
if(NOT ANDROID)
    add_executable(xlog_mobile_smoke ${CMAKE_CURRENT_LIST_DIR}/smoke/host_smoke.cpp)
    target_link_libraries(xlog_mobile_smoke PRIVATE xlog_mobile)
endif()
