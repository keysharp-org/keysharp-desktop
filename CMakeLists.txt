cmake_minimum_required(VERSION 3.20)

project(keysharp-desktop
    VERSION 0.2.0
    DESCRIPTION "Per-application desktop capability broker for Linux"
    HOMEPAGE_URL "https://github.com/keysharp-org/keysharp-desktop"
    LANGUAGES C)

include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)

set(KEYSHARP_DESKTOP_CAPTURE_WORKER_PATH
    "${CMAKE_INSTALL_FULL_LIBEXECDIR}/keysharp-desktop-capture-worker"
    CACHE FILEPATH "root-only capture worker execution path")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)

set(KEYSHARP_PERMISSIONS_SOURCE_DIR "" CACHE PATH
    "keysharp-permissions source directory")
if(KEYSHARP_PERMISSIONS_SOURCE_DIR STREQUAL "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/keysharp-permissions/CMakeLists.txt")
        set(KEYSHARP_PERMISSIONS_SOURCE_DIR
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/keysharp-permissions")
    else()
        message(FATAL_ERROR
            "Set KEYSHARP_PERMISSIONS_SOURCE_DIR to a pinned keysharp-permissions checkout")
    endif()
endif()
add_subdirectory("${KEYSHARP_PERMISSIONS_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/keysharp-permissions" EXCLUDE_FROM_ALL)

add_library(keysharp-desktop-wire STATIC
    src/protocol_io.c
    src/transport.c)
set_target_properties(keysharp-desktop-wire PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden)
target_include_directories(keysharp-desktop-wire PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(keysharp-desktop-wire PUBLIC
    _GNU_SOURCE _POSIX_C_SOURCE=200809L)

add_library(keysharp-desktop-client SHARED
    src/client.c)
add_library(KeysharpDesktop::client ALIAS keysharp-desktop-client)
set_target_properties(keysharp-desktop-client PROPERTIES
    OUTPUT_NAME keysharp-desktop
    EXPORT_NAME client
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
target_include_directories(keysharp-desktop-client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(keysharp-desktop-client PRIVATE
    KEYSHARP_DESKTOP_CLIENT_BUILD _GNU_SOURCE _POSIX_C_SOURCE=200809L)
target_link_libraries(keysharp-desktop-client PRIVATE
    keysharp-desktop-wire KeysharpPermissions::permissions Threads::Threads)

add_library(keysharp-desktop-common STATIC
    src/backend.c
    src/operation_result.c)
target_include_directories(keysharp-desktop-common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(keysharp-desktop-common PUBLIC _GNU_SOURCE _POSIX_C_SOURCE=200809L)
target_link_libraries(keysharp-desktop-common PUBLIC
    keysharp-desktop-wire KeysharpPermissions::permissions PkgConfig::GIO)

add_executable(keysharp-desktop
    src/main.c
    src/session_backend.c
    src/provider.c
    src/authorityd.c
    src/cli.c)

add_library(keysharp-desktop-capture STATIC
    src/capture_worker.c
    src/local_capture.c)
target_include_directories(keysharp-desktop-capture PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(keysharp-desktop-capture PUBLIC
    _GNU_SOURCE _POSIX_C_SOURCE=200809L
    KSD_CAPTURE_WORKER_PATH="${KEYSHARP_DESKTOP_CAPTURE_WORKER_PATH}")
target_link_libraries(keysharp-desktop-capture PUBLIC
    keysharp-desktop-common PkgConfig::GIO)

add_executable(keysharp-desktop-capture-worker
    src/capture_worker_entry.c)
target_link_libraries(keysharp-desktop-capture-worker PRIVATE
    keysharp-desktop-capture)
target_link_libraries(keysharp-desktop PRIVATE
    keysharp-desktop-common keysharp-desktop-client keysharp-desktop-capture
    PkgConfig::GIO Threads::Threads)
set(KSD_PKCHECK_PATH "/usr/bin/pkcheck" CACHE FILEPATH "absolute path to pkcheck")
target_compile_definitions(keysharp-desktop PRIVATE
    KSD_PKCHECK_PATH="${KSD_PKCHECK_PATH}")

foreach(target keysharp-desktop-wire keysharp-desktop-client
               keysharp-desktop-common keysharp-desktop-capture
               keysharp-desktop keysharp-desktop-capture-worker)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2)
    endif()
endforeach()

set(KEYSHARP_DESKTOP_SYSTEMD_SYSTEM_DIR "lib/systemd/system"
    CACHE PATH "system systemd unit directory")
set(KEYSHARP_DESKTOP_SYSTEMD_USER_DIR "lib/systemd/user"
    CACHE PATH "user systemd unit directory")
if(CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
    set(KEYSHARP_DESKTOP_DEFAULT_POLKIT_ACTION_DIR "/usr/share/polkit-1/actions")
else()
    set(KEYSHARP_DESKTOP_DEFAULT_POLKIT_ACTION_DIR
        "${CMAKE_INSTALL_DATADIR}/polkit-1/actions")
endif()
set(KEYSHARP_DESKTOP_POLKIT_ACTION_DIR
    "${KEYSHARP_DESKTOP_DEFAULT_POLKIT_ACTION_DIR}"
    CACHE STRING "polkit action install destination")

configure_file(data/keysharp-desktop.service.in keysharp-desktop.service @ONLY)
configure_file(data/keysharp-desktop-authority.service.in keysharp-desktop-authority.service @ONLY)
configure_file(data/org.keysharp.DesktopCapture.desktop.in org.keysharp.DesktopCapture.desktop @ONLY)
configure_file(data/keysharp-desktop.pc.in keysharp-desktop.pc @ONLY)
configure_package_config_file(
    cmake/KeysharpDesktopConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/KeysharpDesktopConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KeysharpDesktop)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/KeysharpDesktopConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(TARGETS keysharp-desktop keysharp-desktop-client
    EXPORT KeysharpDesktopTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS keysharp-desktop-capture-worker
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/keysharp-desktop.service
    DESTINATION ${KEYSHARP_DESKTOP_SYSTEMD_USER_DIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/keysharp-desktop-authority.service
              data/keysharp-desktop-authority.socket
    DESTINATION ${KEYSHARP_DESKTOP_SYSTEMD_SYSTEM_DIR})
install(FILES data/keysharp-desktop-permissions.conf DESTINATION lib/tmpfiles.d)
install(FILES data/org.keysharp.desktop.policy DESTINATION ${KEYSHARP_DESKTOP_POLKIT_ACTION_DIR})
install(FILES include/keysharp_desktop/client.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/keysharp_desktop)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/keysharp-desktop.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/KeysharpDesktopConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/KeysharpDesktopConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KeysharpDesktop)
install(EXPORT KeysharpDesktopTargets
    FILE KeysharpDesktopTargets.cmake
    NAMESPACE KeysharpDesktop::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KeysharpDesktop)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.keysharp.DesktopCapture.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(DIRECTORY providers/gnome/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/keysharp@keysharp.io)
install(DIRECTORY providers/cinnamon/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/cinnamon/extensions/keysharp@keysharp.io)
install(FILES LICENSE README.md SECURITY.md PROVENANCE.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/keysharp-desktop)
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/keysharp-desktop/docs)
if(CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
    install(PROGRAMS uninstall.sh
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/keysharp-desktop)
endif()

if(BUILD_TESTING)
    add_executable(protocol-tests tests/protocol_tests.c)
    target_link_libraries(protocol-tests PRIVATE keysharp-desktop-wire)
    add_test(NAME protocol-tests COMMAND protocol-tests)

    add_executable(authority-fd-tests tests/authority_fd_tests.c)
    target_link_libraries(authority-fd-tests PRIVATE keysharp-desktop-wire)
    add_test(NAME authority-fd-tests COMMAND authority-fd-tests)

    add_executable(client-abi-tests tests/client_abi_tests.c)
    target_include_directories(client-abi-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-abi-tests PRIVATE keysharp-desktop-client)
    add_test(NAME client-abi-tests COMMAND client-abi-tests)

    add_executable(client-stream-tests
        tests/client_stream_tests.c
        src/client.c)
    target_include_directories(client-stream-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(client-stream-tests PRIVATE
        KSD_CLIENT_TESTING _GNU_SOURCE _POSIX_C_SOURCE=200809L)
    target_link_libraries(client-stream-tests PRIVATE
        keysharp-desktop-wire KeysharpPermissions::permissions Threads::Threads)
    add_test(NAME client-stream-tests COMMAND client-stream-tests)

    add_test(NAME client-exported-symbols-test
        COMMAND ${CMAKE_COMMAND}
            -DLIBRARY=$<TARGET_FILE:keysharp-desktop-client>
            -DCLIENT_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/include/keysharp_desktop/client.h
            -DNM=${CMAKE_NM}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/exported_symbols_test.cmake)

    add_test(NAME install-tree-consumer-test
        COMMAND sh
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_tree_consumer_test.sh
            ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_INSTALL_PREFIX}
            ${CMAKE_INSTALL_LIBDIR})

    add_executable(capture-pipe-security-tests
        tests/capture_pipe_security_tests.c)
    target_link_libraries(capture-pipe-security-tests PRIVATE
        keysharp-desktop-capture)
    add_test(NAME capture-pipe-security-tests
        COMMAND capture-pipe-security-tests)
    set_tests_properties(capture-pipe-security-tests PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(permissions-core-tests
        ${KEYSHARP_PERMISSIONS_SOURCE_DIR}/tests/identity_tests.c
        ${KEYSHARP_PERMISSIONS_SOURCE_DIR}/tests/polkit_tests.c
        ${KEYSHARP_PERMISSIONS_SOURCE_DIR}/tests/scopes_tests.c
        ${KEYSHARP_PERMISSIONS_SOURCE_DIR}/tests/store_tests.c
        ${KEYSHARP_PERMISSIONS_SOURCE_DIR}/tests/test_main.c)
    target_include_directories(permissions-core-tests PRIVATE
        ${KEYSHARP_PERMISSIONS_SOURCE_DIR}/tests)
    target_compile_definitions(permissions-core-tests PRIVATE
        _GNU_SOURCE _POSIX_C_SOURCE=200809L)
    target_link_libraries(permissions-core-tests PRIVATE
        KeysharpPermissions::permissions)
    add_test(NAME permissions-core-tests COMMAND permissions-core-tests)

    add_executable(permission-domain-tests tests/permission_domain_tests.c)
    target_link_libraries(permission-domain-tests PRIVATE
        KeysharpPermissions::permissions)
    target_include_directories(permission-domain-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(permission-domain-tests PRIVATE
        _GNU_SOURCE _POSIX_C_SOURCE=200809L)
    add_test(NAME permission-domain-tests COMMAND permission-domain-tests)

    # Both build their scratch store under $HOME, because a store refuses any parent a
    # third party can write to and /tmp is exactly that. A build sandbox points HOME at
    # a directory that does not exist, so give them one in the build tree instead.
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test-home)
    set_tests_properties(permissions-core-tests permission-domain-tests PROPERTIES
        ENVIRONMENT HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home)

    add_test(NAME version-command COMMAND keysharp-desktop version)
    set_tests_properties(version-command PROPERTIES
        PASS_REGULAR_EXPRESSION "^keysharp-desktop 0.2.0")
    add_test(NAME info-command COMMAND keysharp-desktop info)
    set_tests_properties(info-command PROPERTIES
        PASS_REGULAR_EXPRESSION "client_abi_minor=1")
    add_test(NAME unknown-command-rejected
        COMMAND keysharp-desktop unknown-command)
    set_tests_properties(unknown-command-rejected PROPERTIES WILL_FAIL TRUE)

    add_test(NAME bare-revoke-rejected COMMAND keysharp-desktop revoke)
    set_tests_properties(bare-revoke-rejected PROPERTIES WILL_FAIL TRUE)

    add_test(NAME provider-gate-test
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/provider_gate_test.cmake)

    add_test(NAME release-layout-test
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/release_layout_test.cmake)

    add_test(NAME portable-compatibility-tests
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/portable_compatibility_tests.sh
            ${CMAKE_CURRENT_SOURCE_DIR})

    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        foreach(test_target
                protocol-tests
                authority-fd-tests
                client-abi-tests
                client-stream-tests
                capture-pipe-security-tests
                permission-domain-tests)
            target_compile_options(${test_target} PRIVATE -UNDEBUG)
        endforeach()
    endif()
endif()

set(CPACK_PACKAGE_NAME "keysharp-desktop")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Descolada")
set(CPACK_PACKAGE_CONTACT "Descolada <16986957+Descolada@users.noreply.github.com>")
set(CPACK_PACKAGE_FILE_NAME "keysharp-desktop-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libglib2.0-0 (>= 2.56) | libglib2.0-0t64, polkitd | policykit-1, systemd")
set(CPACK_DEBIAN_PACKAGE_PROVIDES "keysharp-desktop-client-abi-0")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_BREAKS "keysharp (<< 0.0.0.17)")
set(CPACK_DEBIAN_PACKAGE_REPLACES "keysharp (<< 0.0.0.17)")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(KEYSHARP_DESKTOP_DEBIAN_CONTROLS)
foreach(script preinst postinst prerm postrm)
    configure_file(packaging/debian/${script} ${script} COPYONLY)
    file(CHMOD ${CMAKE_CURRENT_BINARY_DIR}/${script} PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    list(APPEND KEYSHARP_DESKTOP_DEBIAN_CONTROLS ${CMAKE_CURRENT_BINARY_DIR}/${script})
endforeach()
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${KEYSHARP_DESKTOP_DEBIAN_CONTROLS}")
include(CPack)
