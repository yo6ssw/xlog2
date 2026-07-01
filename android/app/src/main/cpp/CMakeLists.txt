# JNI library for the Android app: wraps the carved-out xlog_mobile sync core
# (android/native) and exposes it to Kotlin's XlogCore. Invoked by Gradle's
# externalNativeBuild; ANDROID_ABI / CMAKE_ANDROID_* come from the NDK toolchain.
cmake_minimum_required(VERSION 3.18)
project(xlog2jni LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build the carve-out (defines target xlog_mobile + multimaster, wires the
# vendored per-ABI libsodium/libcurl/sqlite under android/third_party/<abi>).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../../native
                 ${CMAKE_BINARY_DIR}/xlog_mobile)

add_library(xlog2jni SHARED xlog_jni.cpp opus_jni.cpp paddle_jni.cpp)
target_link_libraries(xlog2jni PRIVATE xlog_mobile log)

# Vendored libopus for the rig-audio decoder (opus_jni.cpp). Same per-ABI layout
# as libsodium, produced by android/third_party/build-deps.sh.
set(XLOG_OPUS "${CMAKE_CURRENT_LIST_DIR}/../../../../third_party/${ANDROID_ABI}")
if(NOT EXISTS "${XLOG_OPUS}/lib/libopus.a")
    message(FATAL_ERROR
        "Missing ${XLOG_OPUS}/lib/libopus.a — run android/third_party/build-deps.sh first")
endif()
target_include_directories(xlog2jni PRIVATE ${XLOG_OPUS}/include)
target_link_libraries(xlog2jni PRIVATE ${XLOG_OPUS}/lib/libopus.a)

# 16 KB page-size compatibility (Android 15+ devices, e.g. recent Samsung/Pixel).
# NDK r26 still defaults LOAD segments to 4 KB (0x1000); force 16 KB (0x4000) so
# the .so loads on 16 KB-page devices. NDK r27+ does this by default — harmless
# to keep. Verify with: llvm-readelf -l libxlog2jni.so  (Align must be 0x4000).
target_link_options(xlog2jni PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384")

# Match the QRZ toggle of the carve-out so the bridge #ifdefs line up.
if(NOT XLOG_MOBILE_QRZ)
    target_compile_definitions(xlog2jni PRIVATE XLOG_NO_QRZ=1)
endif()

# Strip in release to keep the .so small.
target_link_options(xlog2jni PRIVATE $<$<CONFIG:Release>:-s>)
