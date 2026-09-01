cmake_minimum_required(VERSION 3.20)
project(keysharp_desktop_consumer LANGUAGES C)

find_package(KeysharpDesktop CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(KeysharpDesktopPkg REQUIRED IMPORTED_TARGET keysharp-desktop)

add_executable(cmake-consumer main.c)
target_link_libraries(cmake-consumer PRIVATE KeysharpDesktop::client)

add_executable(pkg-config-consumer main.c)
target_link_libraries(pkg-config-consumer PRIVATE PkgConfig::KeysharpDesktopPkg)
