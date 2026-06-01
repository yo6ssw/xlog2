cmake_minimum_required(VERSION 3.16)
project(xlog2
    VERSION 0.1.0
    DESCRIPTION "A GTK4 amateur-radio logging program (xlog clone)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(HAMLIB REQUIRED IMPORTED_TARGET hamlib)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)

add_executable(xlog2
    src/main.cpp
    src/XlogApplication.cpp
    src/MainWindow.cpp
    src/LogPage.cpp
    src/LogBook.cpp
    src/Adif.cpp
    src/Bands.cpp
    src/Udp.cpp
    src/Rig.cpp
    src/Lotw.cpp
    src/Qrz.cpp
    src/CwKeyer.cpp
)

target_include_directories(xlog2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(xlog2 PRIVATE
    PkgConfig::GTKMM
    PkgConfig::SQLITE
    PkgConfig::HAMLIB
    PkgConfig::CURL
    Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(xlog2 PRIVATE -Wall -Wextra)
endif()

install(TARGETS xlog2 RUNTIME DESTINATION bin)
