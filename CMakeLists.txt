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
set(KEYSHARP_DESKTOP_KWIN_SCRIPT_PATH
    "${CMAKE_INSTALL_FULL_DATADIR}/kwin/scripts/io.github.keysharp.desktop.kwin/contents/code/main.js"
    CACHE FILEPATH "installed KWin provider script path")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/keysharp_desktop/client.h"
    KEYSHARP_DESKTOP_ABI_MINOR_LINE REGEX "^#define KSD_CLIENT_ABI_MINOR ")
string(REGEX MATCH "[0-9]+" KEYSHARP_DESKTOP_ABI_MINOR
    "${KEYSHARP_DESKTOP_ABI_MINOR_LINE}")

set(KEYSHARP_DESKTOP_PROVIDER_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/providers")
set(KEYSHARP_DESKTOP_PROVIDER_OUTPUTS
    "${KEYSHARP_DESKTOP_PROVIDER_GENERATED_DIR}/gnome/extension.js"
    "${KEYSHARP_DESKTOP_PROVIDER_GENERATED_DIR}/cinnamon/extension.js")
set(KEYSHARP_DESKTOP_PROVIDER_INPUTS
    "${CMAKE_CURRENT_SOURCE_DIR}/providers/shared/provider-core.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/providers/shared/gnome-shim.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/providers/shared/cinnamon-shim.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/interfaces/private/org.keysharp.Desktop.Provider1.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/providers/shared/generate.cmake")
add_custom_command(
    OUTPUT ${KEYSHARP_DESKTOP_PROVIDER_OUTPUTS}
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DOUTPUT_DIR=${KEYSHARP_DESKTOP_PROVIDER_GENERATED_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/providers/shared/generate.cmake
    DEPENDS ${KEYSHARP_DESKTOP_PROVIDER_INPUTS}
    COMMENT "Generating shell provider extensions"
    VERBATIM)
add_custom_target(generate-provider-extensions ALL
    DEPENDS ${KEYSHARP_DESKTOP_PROVIDER_OUTPUTS})
add_custom_target(regenerate-provider-extensions
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DOUTPUT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/providers
        -P ${CMAKE_CURRENT_SOURCE_DIR}/providers/shared/generate.cmake
    DEPENDS ${KEYSHARP_DESKTOP_PROVIDER_INPUTS}
    COMMENT "Regenerating committed shell provider extensions"
    VERBATIM)

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
    src/session_environ.c
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
    src/operation_result.c
    src/operation_scope.c
    src/kwin_lanes.c
    src/kwin_bus.c
    src/kwin_relay.c
    src/kwin_jobs.c
    src/kwin_peer.c
    src/kwin_envelope.c
    src/install_mode.c
    src/worker_pool.c)
target_include_directories(keysharp-desktop-common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(keysharp-desktop-common PUBLIC
    _GNU_SOURCE _POSIX_C_SOURCE=200809L
    KSD_KWIN_SCRIPT_PATH="${KEYSHARP_DESKTOP_KWIN_SCRIPT_PATH}")
target_link_libraries(keysharp-desktop-common PUBLIC
    keysharp-desktop-wire KeysharpPermissions::permissions PkgConfig::GIO)

add_executable(keysharp-desktop
    src/main.c
    src/session_backend.c
    src/provider.c
    src/authorityd.c
    src/enable_extension.c
    src/cli.c)
# Where this build put the extension, so the command can tell the user whether
# the running shell searches there. Taken from the same variable the install
# rule uses rather than assumed to be /usr/local.
target_compile_definitions(keysharp-desktop PRIVATE
    KSD_EXTENSION_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}")

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
    keysharp-desktop-common keysharp-desktop-x11 keysharp-desktop-wayland
    PkgConfig::GIO)

# Linked into the worker executable and nothing else. The whole argument that
# root never touches X rests on this one edge, so it is a separate target
# rather than a few files added to an existing library.
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET
    xcb>=1.14 xcb-shm>=1.14 xcb-randr>=1.14 xcb-shape>=1.14 xcb-xkb>=1.14 xcb-composite>=1.14
    xkbcommon xkbcommon-x11)
add_library(keysharp-desktop-x11 STATIC
    src/x11/x11_display.c
    src/x11/x11_connect.c
    src/x11/x11_query.c
    src/x11/x11_windows.c
    src/x11/x11_extended.c
    src/x11/x11_watch.c
    src/x11/x11_capture.c
    src/x11/x11_clipboard.c
    src/x11/x11_control.c
    src/x11/x11_worker.c)
target_include_directories(keysharp-desktop-x11 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/x11)
target_compile_definitions(keysharp-desktop-x11 PUBLIC
    _GNU_SOURCE _POSIX_C_SOURCE=200809L)
target_link_libraries(keysharp-desktop-x11 PUBLIC
    keysharp-desktop-wire PkgConfig::XCB)

# The Wayland backend for compositors with no extension of their own. Standard
# protocol bindings come from wayland-protocols; compositor-specific protocols
# are pinned below because distributions do not package their XML consistently.
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client>=1.20)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(WAYLAND_SCANNER REQUIRED wayland-scanner)
pkg_get_variable(WAYLAND_SCANNER_BIN wayland-scanner wayland_scanner)

# Distributions lag the protocol definitions badly -- Ubuntu 24.04 ships 1.34,
# Debian 12 ships 1.31 -- and the package is data only, so demanding a
# system-wide upgrade would be a heavy answer to a directory of XML. Pointing at
# a newer checkout supplies the standard definitions without requiring a
# system-wide package upgrade.
#
# The version floor is therefore checked only when there is no override to
# check instead. Demanding it first would make the escape hatch unreachable on
# exactly the machines that need it.
set(KEYSHARP_DESKTOP_WAYLAND_PROTOCOL_DIR "" CACHE PATH
    "wayland-protocols data directory, overriding the installed one")
if(KEYSHARP_DESKTOP_WAYLAND_PROTOCOL_DIR STREQUAL "")
    # 1.39 is where ext-data-control-v1 arrived.
    pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.39)
    pkg_get_variable(WAYLAND_PROTOCOL_DIR wayland-protocols pkgdatadir)
else()
    set(WAYLAND_PROTOCOL_DIR "${KEYSHARP_DESKTOP_WAYLAND_PROTOCOL_DIR}")
endif()

set(KSD_WAYLAND_GENERATED)
foreach(spec
        "staging/ext-data-control/ext-data-control-v1"
        "staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1"
        "staging/ext-image-capture-source/ext-image-capture-source-v1"
        "staging/ext-image-copy-capture/ext-image-copy-capture-v1"
        "unstable/xdg-output/xdg-output-unstable-v1")
    get_filename_component(stem "${spec}" NAME)
    set(xml "${WAYLAND_PROTOCOL_DIR}/${spec}.xml")
    if(NOT EXISTS "${xml}")
        message(FATAL_ERROR
            "wayland protocol missing: ${xml}
"
            "The installed wayland-protocols does not carry ${spec}. It is a "
            "data-only package: clone a newer copy and configure with "
            "-DKEYSHARP_DESKTOP_WAYLAND_PROTOCOL_DIR=/path/to/wayland-protocols")
    endif()
    set(header "${CMAKE_CURRENT_BINARY_DIR}/${stem}-client-protocol.h")
    set(source "${CMAKE_CURRENT_BINARY_DIR}/${stem}-protocol.c")
    add_custom_command(OUTPUT "${header}"
        COMMAND ${WAYLAND_SCANNER_BIN} client-header "${xml}" "${header}"
        DEPENDS "${xml}" VERBATIM)
    add_custom_command(OUTPUT "${source}"
        COMMAND ${WAYLAND_SCANNER_BIN} private-code "${xml}" "${source}"
        DEPENDS "${xml}" VERBATIM)
    list(APPEND KSD_WAYLAND_GENERATED "${header}" "${source}")
    # The tests stand up a real Wayland server to talk to, so the server side
    # of the same XML is generated too. Same file, same version, no chance of
    # the two halves of the test disagreeing about the protocol.
    set(server_header "${CMAKE_CURRENT_BINARY_DIR}/${stem}-server-protocol.h")
    add_custom_command(OUTPUT "${server_header}"
        COMMAND ${WAYLAND_SCANNER_BIN} server-header "${xml}" "${server_header}"
        DEPENDS "${xml}" VERBATIM)
    list(APPEND KSD_WAYLAND_SERVER_GENERATED "${server_header}")
endforeach()

# wlroots protocols are vendored because distributions rarely package them.
foreach(spec
        "wlr-foreign-toplevel-management-unstable-v1"
        "wlr-screencopy-unstable-v1"
        "wlr-virtual-pointer-unstable-v1")
    set(xml "${CMAKE_CURRENT_SOURCE_DIR}/protocols/${spec}.xml")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/${spec}-client-protocol.h")
    set(source "${CMAKE_CURRENT_BINARY_DIR}/${spec}-protocol.c")
    set(server_header
        "${CMAKE_CURRENT_BINARY_DIR}/${spec}-server-protocol.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND ${WAYLAND_SCANNER_BIN} client-header "${xml}" "${header}"
        DEPENDS "${xml}" VERBATIM)
    add_custom_command(OUTPUT "${source}"
        COMMAND ${WAYLAND_SCANNER_BIN} private-code "${xml}" "${source}"
        DEPENDS "${xml}" VERBATIM)
    add_custom_command(OUTPUT "${server_header}"
        COMMAND ${WAYLAND_SCANNER_BIN} server-header "${xml}"
            "${server_header}"
        DEPENDS "${xml}" VERBATIM)
    list(APPEND KSD_WAYLAND_GENERATED "${header}" "${source}")
    list(APPEND KSD_WAYLAND_SERVER_GENERATED "${server_header}")
endforeach()

# COSMIC's version-2 toplevel extension enriches the standard ext list with
# state and geometry; its version-1 manager supplies the matching actions.
# The vendored XML files are wire-compatible subsets so old distro scanners
# can build them without pulling in unused workspace protocols.
foreach(spec
        "cosmic-toplevel-info-v2"
        "cosmic-toplevel-management-v1")
    set(xml "${CMAKE_CURRENT_SOURCE_DIR}/protocols/${spec}.xml")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/${spec}-client-protocol.h")
    set(source "${CMAKE_CURRENT_BINARY_DIR}/${spec}-protocol.c")
    set(server_header
        "${CMAKE_CURRENT_BINARY_DIR}/${spec}-server-protocol.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND ${WAYLAND_SCANNER_BIN} client-header "${xml}" "${header}"
        DEPENDS "${xml}" VERBATIM)
    add_custom_command(OUTPUT "${source}"
        COMMAND ${WAYLAND_SCANNER_BIN} private-code "${xml}" "${source}"
        DEPENDS "${xml}" VERBATIM)
    add_custom_command(OUTPUT "${server_header}"
        COMMAND ${WAYLAND_SCANNER_BIN} server-header "${xml}"
            "${server_header}"
        DEPENDS "${xml}" VERBATIM)
    list(APPEND KSD_WAYLAND_GENERATED "${header}" "${source}")
    list(APPEND KSD_WAYLAND_SERVER_GENERATED "${server_header}")
endforeach()

add_library(keysharp-desktop-wayland STATIC
    src/wayland/portal_capture.c
    src/wayland/wl_connect.c
    src/wayland/wl_cosmic_windows.c
    src/wayland/wl_hypr.c
    src/wayland/wl_keyboard.c
    src/wayland/wl_clipboard.c
    src/wayland/wl_capture.c
    src/wayland/wl_outputs.c
    src/wayland/wl_pointer.c
    src/wayland/wl_windows.c
    src/wayland/wl_wlr_windows.c
    src/wayland/wl_worker.c
    ${KSD_WAYLAND_GENERATED})
target_include_directories(keysharp-desktop-wayland PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wayland
    ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(keysharp-desktop-wayland PUBLIC
    _GNU_SOURCE _POSIX_C_SOURCE=200809L)
target_link_libraries(keysharp-desktop-wayland PUBLIC
    keysharp-desktop-wire PkgConfig::WAYLAND PkgConfig::XKBCOMMON PkgConfig::GIO)


add_executable(keysharp-desktop-capture-worker
    src/capture_worker_entry.c)
target_link_libraries(keysharp-desktop-capture-worker PRIVATE
    keysharp-desktop-capture keysharp-desktop-x11
    keysharp-desktop-wayland)
target_link_libraries(keysharp-desktop PRIVATE
    keysharp-desktop-common keysharp-desktop-client keysharp-desktop-capture
    PkgConfig::GIO Threads::Threads)
set(KSD_PKCHECK_PATH "/usr/bin/pkcheck" CACHE FILEPATH "absolute path to pkcheck")
target_compile_definitions(keysharp-desktop PRIVATE
    KSD_PKCHECK_PATH="${KSD_PKCHECK_PATH}")

foreach(target keysharp-desktop-wire keysharp-desktop-client
               keysharp-desktop-common keysharp-desktop-capture
               keysharp-desktop-x11 keysharp-desktop-wayland
               keysharp-desktop keysharp-desktop-capture-worker)
    # keysharp-desktop carries src/enable_extension.c, so it is covered here.
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
configure_file(data/keysharp-desktop-authority-user.service.in
    keysharp-desktop-authority-user.service @ONLY)
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

# Both executables have to find libkeysharp-desktop wherever the tree was put.
# A system installation gets this for free, because its library lands in a
# directory the loader already searches; a user installation does not, and
# without an rpath the daemon cannot start at all outside /usr.
#
# Relative to the binary rather than absolute, so the tree can be moved or
# installed anywhere. This is not the usual risk that carries -- neither binary
# is setuid, and the directory it resolves to is owned by whoever the
# installation belongs to: root for a system installation, and for a user one,
# the same user the authority already runs as.
set_target_properties(keysharp-desktop keysharp-desktop-capture-worker
    PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
        INSTALL_RPATH_USE_LINK_PATH FALSE)

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
# The user authority goes beside the session daemon's own user unit. Installing
# both units always, and starting only one, is deliberate: which installation
# this is depends on where it was installed and who runs it, and that is not
# known at build time. The two cannot both serve, because the client prefers
# the system socket wherever one exists.
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/keysharp-desktop-authority-user.service
    data/keysharp-desktop-authority-user.socket
    DESTINATION ${KEYSHARP_DESKTOP_SYSTEMD_USER_DIR})
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
install(FILES
    "${KEYSHARP_DESKTOP_PROVIDER_GENERATED_DIR}/gnome/extension.js"
    providers/gnome/metadata.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/keysharp@keysharp.io)
install(FILES
    "${KEYSHARP_DESKTOP_PROVIDER_GENERATED_DIR}/cinnamon/extension.js"
    providers/cinnamon/metadata.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/cinnamon/extensions/keysharp@keysharp.io)
# A KPackage, not an extension, so it goes where Scripting::start() looks and is
# named by the KPlugin.Id in its manifest rather than by a uuid directory.
install(DIRECTORY providers/kwin/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/kwin/scripts/io.github.keysharp.desktop.kwin)
install(FILES LICENSE README.md SECURITY.md PROVENANCE.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/keysharp-desktop)
install(DIRECTORY examples/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/keysharp-desktop/examples)
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/keysharp-desktop/docs)
if(CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
    install(PROGRAMS uninstall.sh
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/keysharp-desktop)
endif()

# A direct root install finishes the job, so `sudo cmake --install` needs no
# follow-up commands. Packaging installs stage into DESTDIR, or turn this off,
# and run the equivalent step from their own maintainer scripts. Only the system
# side is done here: refreshing an already-running user session needs the
# invoking user's bus, which install.sh resolves.
option(KEYSHARP_DESKTOP_SETUP_ON_INSTALL
    "refresh the linker cache and enable the services after a direct root install" ON)
if(KEYSHARP_DESKTOP_SETUP_ON_INSTALL)
    install(CODE "
        execute_process(COMMAND id -u OUTPUT_VARIABLE _install_uid
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(\"\$ENV{DESTDIR}\" STREQUAL \"\" AND _install_uid STREQUAL \"0\")
            find_program(_ldconfig ldconfig PATHS /sbin /usr/sbin NO_CACHE)
            if(_ldconfig)
                execute_process(COMMAND \"\${_ldconfig}\")
            endif()
            find_program(_tmpfiles systemd-tmpfiles NO_CACHE)
            if(_tmpfiles)
                execute_process(COMMAND \"\${_tmpfiles}\" --create
                    \"\${CMAKE_INSTALL_PREFIX}/lib/tmpfiles.d/keysharp-desktop-permissions.conf\"
                    RESULT_VARIABLE _tmpfiles_result)
                if(NOT _tmpfiles_result EQUAL 0)
                    message(WARNING \"systemd-tmpfiles did not create the keysharp-desktop permission store\")
                endif()
            endif()
            find_program(_systemctl systemctl NO_CACHE)
            if(_systemctl)
                execute_process(COMMAND \"\${_systemctl}\" daemon-reload)
                execute_process(COMMAND \"\${_systemctl}\" enable --now
                    keysharp-desktop-authority.socket
                    RESULT_VARIABLE _socket_result)
                execute_process(COMMAND \"\${_systemctl}\" try-restart
                    keysharp-desktop-authority.service)
                execute_process(COMMAND \"\${_systemctl}\" --global enable
                    keysharp-desktop.service
                    RESULT_VARIABLE _user_result)
                if(NOT _socket_result EQUAL 0 OR NOT _user_result EQUAL 0)
                    message(WARNING \"keysharp-desktop service setup was not completed; check 'systemctl status keysharp-desktop-authority.socket'\")
                endif()
            else()
                message(WARNING \"systemctl was not found; the keysharp-desktop services were not enabled\")
            endif()
            # The one step that cannot be done from here. Enabling the shell
            # extension writes the invoking user's own dconf, which needs their
            # session bus; install.sh resolves that, and this does not. Left
            # unsaid, the install looks complete and every client is then told
            # the backend is None -- which names no cause and sends people
            # looking at the daemon, the socket and the provider in turn.
            message(STATUS
                \"keysharp-desktop: on GNOME or Cinnamon, finish the install by running this as yourself, not with sudo:\")
            message(STATUS \"    keysharp-desktop enable-extension\")
        endif()
    ")
endif()

if(BUILD_TESTING)
    # Assertions stay live here whatever the build type.
    #
    # Every test in this tree is written on assert(), and most put the call
    # under test inside the assertion -- assert(socketpair(...) == 0) and the
    # like. CMAKE_BUILD_TYPE=Release, which CI uses, adds -DNDEBUG, and NDEBUG
    # does not merely skip the check: it deletes the expression. The socketpair
    # is never created, the pipe never opened, the child never forked. Two
    # suites then crash on uninitialised descriptors and the rest pass having
    # executed nothing, which is the worse outcome of the two because it is
    # silent.
    #
    # Hoisting 68 calls out of their assertions across ten files would work
    # equally well and is the tidier habit, but it leaves the next one written
    # in the old style to fail the same way. This makes the build type
    # incapable of emptying a test.
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-UNDEBUG)
    endif()

    add_executable(protocol-tests tests/protocol_tests.c)
    target_link_libraries(protocol-tests PRIVATE keysharp-desktop-wire)
    add_test(NAME protocol-tests COMMAND protocol-tests)

    add_executable(provider-xml-tests tests/provider_xml_tests.c)
    target_link_libraries(provider-xml-tests PRIVATE
        keysharp-desktop-common)
    add_test(NAME provider-xml-tests
        COMMAND provider-xml-tests ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(request-chunk-tests tests/request_chunk_tests.c)
    target_link_libraries(request-chunk-tests PRIVATE
        keysharp-desktop-common)
    add_test(NAME request-chunk-tests COMMAND request-chunk-tests)

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

    # Compiles the worker sources rather than linking the library, so the
    # round-trip hook is reachable. The library's objects would be built
    # without KSD_CAPTURE_WORKER_TESTING and the hook would not exist in them.
    add_executable(capture-pipe-security-tests
        tests/capture_pipe_security_tests.c
        src/capture_worker.c
        src/local_capture.c)
    target_include_directories(capture-pipe-security-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_include_directories(capture-pipe-security-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/x11
        ${CMAKE_CURRENT_SOURCE_DIR}/src/wayland)
    target_compile_definitions(capture-pipe-security-tests PRIVATE
        _GNU_SOURCE _POSIX_C_SOURCE=200809L
        KSD_CAPTURE_WORKER_TESTING
        KSD_CAPTURE_WORKER_PATH="${KEYSHARP_DESKTOP_CAPTURE_WORKER_PATH}")
    target_link_libraries(capture-pipe-security-tests PRIVATE
        keysharp-desktop-common keysharp-desktop-x11 keysharp-desktop-wayland
        PkgConfig::GIO)
    add_test(NAME capture-pipe-security-tests
        COMMAND capture-pipe-security-tests)
    set_tests_properties(capture-pipe-security-tests PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(x11-display-tests tests/x11_display_tests.c)
    target_link_libraries(x11-display-tests PRIVATE
        keysharp-desktop-x11 keysharp-desktop-common)
    add_test(NAME x11-display-tests COMMAND x11-display-tests)

    # Compiles the capture source rather than taking it from the library,
    # so the hook that forces the non-shared-memory path is reachable. The
    # library objects are built without KSD_X11_TESTING, and a server that
    # offers MIT-SHM would otherwise never run the fallback at all.
    add_executable(x11-server-tests
        tests/x11_server_tests.c
        src/x11/x11_capture.c
        src/x11/x11_clipboard.c)
    target_compile_definitions(x11-server-tests PRIVATE
        KSD_X11_TESTING _GNU_SOURCE _POSIX_C_SOURCE=200809L)
    target_link_libraries(x11-server-tests PRIVATE
        keysharp-desktop-x11 keysharp-desktop-common)
    add_test(NAME x11-server-tests
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/x11_server_test.sh
            $<TARGET_FILE:x11-server-tests>)
    set_tests_properties(x11-server-tests PROPERTIES SKIP_RETURN_CODE 77)

    # The persistent worker's serving loop is a static function reached through
    # the same testing hook as the bootstrap parser, so this target compiles
    # capture_worker.c itself rather than linking the library, whose objects are
    # built without KSD_CAPTURE_WORKER_TESTING. It runs under the same X server
    # wrapper as x11-server-tests: the loop opens a real display, and proving it
    # opens exactly one is the point.
    add_executable(persistent-worker-tests
        tests/persistent_worker_tests.c
        src/capture_worker.c
        src/local_capture.c)
    target_include_directories(persistent-worker-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/x11
        ${CMAKE_CURRENT_SOURCE_DIR}/src/wayland)
    target_compile_definitions(persistent-worker-tests PRIVATE
        _GNU_SOURCE _POSIX_C_SOURCE=200809L
        KSD_CAPTURE_WORKER_TESTING
        KSD_CAPTURE_WORKER_PATH="${KEYSHARP_DESKTOP_CAPTURE_WORKER_PATH}")
    target_link_libraries(persistent-worker-tests PRIVATE
        keysharp-desktop-common keysharp-desktop-x11 keysharp-desktop-wayland
        PkgConfig::GIO)
    add_test(NAME persistent-worker-tests
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/x11_server_test.sh
            $<TARGET_FILE:persistent-worker-tests>)
    set_tests_properties(persistent-worker-tests PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(x11-bench-tests tests/x11_bench_tests.c)
    target_link_libraries(x11-bench-tests PRIVATE
        keysharp-desktop-x11 keysharp-desktop-common)

    # The command needs a bus, a shell and an installed schema; its decisions
    # do not. Compiling the source in directly gives real assertions in a
    # container that has none of the three, where a source-invariant grep would
    # only prove the text is present.
    add_executable(enable-extension-tests
        tests/enable_extension_tests.c
        src/enable_extension.c)
    target_include_directories(enable-extension-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(enable-extension-tests PRIVATE
        KSD_EXTENSION_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}")
    target_link_libraries(enable-extension-tests PRIVATE
        keysharp-desktop-common PkgConfig::GIO)
    add_test(NAME enable-extension-tests COMMAND enable-extension-tests)

    add_executable(install-mode-tests tests/install_mode_tests.c)
    target_link_libraries(install-mode-tests PRIVATE keysharp-desktop-common)
    add_test(NAME install-mode-tests COMMAND install-mode-tests)

    add_executable(worker-pool-tests tests/worker_pool_tests.c)
    target_link_libraries(worker-pool-tests PRIVATE keysharp-desktop-common)
    add_test(NAME worker-pool-tests COMMAND worker-pool-tests)

    add_executable(kwin-envelope-tests tests/kwin_envelope_tests.c)
    target_link_libraries(kwin-envelope-tests PRIVATE keysharp-desktop-common)
    add_test(NAME kwin-envelope-tests COMMAND kwin-envelope-tests)

    add_executable(kwin-lane-tests tests/kwin_lane_tests.c)
    target_link_libraries(kwin-lane-tests PRIVATE keysharp-desktop-common)
    add_test(NAME kwin-lane-tests COMMAND kwin-lane-tests)

    add_executable(kwin-job-tests tests/kwin_job_tests.c)
    target_link_libraries(kwin-job-tests PRIVATE keysharp-desktop-common)
    add_test(NAME kwin-job-tests COMMAND kwin-job-tests)

    add_executable(kwin-host-tests tests/kwin_host_tests.c)
    target_link_libraries(kwin-host-tests PRIVATE keysharp-desktop-common)
    add_test(NAME kwin-host-tests COMMAND kwin-host-tests)

    add_executable(kwin-relay-tests tests/kwin_relay_tests.c)
    target_link_libraries(kwin-relay-tests PRIVATE
        keysharp-desktop-common Threads::Threads)
    add_test(NAME kwin-relay-tests COMMAND kwin-relay-tests)

    # Compiles the clipboard source rather than linking it, so the hook that
    # lowers the deadline is reachable, and links wayland-server so the test
    # can BE the compositor.
    pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
    add_executable(wayland-clipboard-tests
        tests/wayland_clipboard_tests.c
        src/wayland/wl_clipboard.c
        ${KSD_WAYLAND_SERVER_GENERATED})
    target_compile_definitions(wayland-clipboard-tests PRIVATE
        KSD_WAYLAND_TESTING _GNU_SOURCE _POSIX_C_SOURCE=200809L)
    target_link_libraries(wayland-clipboard-tests PRIVATE
        keysharp-desktop-wayland keysharp-desktop-common
        PkgConfig::WAYLAND_SERVER)
    add_test(NAME wayland-clipboard-tests COMMAND wayland-clipboard-tests)
    set_tests_properties(wayland-clipboard-tests PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(portal-capture-tests tests/portal_capture_tests.c)
    target_link_libraries(portal-capture-tests PRIVATE
        keysharp-desktop-wayland keysharp-desktop-common PkgConfig::GIO)
    add_test(NAME portal-capture-tests COMMAND portal-capture-tests)

    add_executable(operation-scope-tests tests/operation_scope_tests.c)
    target_link_libraries(operation-scope-tests PRIVATE
        keysharp-desktop-common)
    add_test(NAME operation-scope-tests COMMAND operation-scope-tests)

    add_executable(backend-session-tests tests/backend_session_tests.c)
    target_link_libraries(backend-session-tests PRIVATE
        keysharp-desktop-common)
    add_test(NAME backend-session-tests COMMAND backend-session-tests)

    add_executable(generic-session-tests
        tests/generic_session_tests.c
        src/authorityd.c
        src/provider.c)
    target_include_directories(generic-session-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(generic-session-tests PRIVATE
        KSD_AUTHORITY_TESTING KSD_PKCHECK_PATH="${KSD_PKCHECK_PATH}")
    target_link_libraries(generic-session-tests PRIVATE
        keysharp-desktop-common keysharp-desktop-capture
        PkgConfig::GIO Threads::Threads)
    add_test(NAME generic-session-tests COMMAND generic-session-tests)
    set_tests_properties(generic-session-tests PROPERTIES
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
    # third party can write to, and /tmp is exactly that. A build sandbox points HOME at
    # a directory that does not exist, so fall back to the build tree there. The build
    # tree is not the first choice: a checkout can sit under a world-writable mount,
    # which the store refuses for the same reason it refuses /tmp.
    if(EXISTS "$ENV{HOME}")
        set(KEYSHARP_DESKTOP_TEST_HOME "$ENV{HOME}")
    else()
        set(KEYSHARP_DESKTOP_TEST_HOME "${CMAKE_CURRENT_BINARY_DIR}/test-home")
        file(MAKE_DIRECTORY "${KEYSHARP_DESKTOP_TEST_HOME}")
    endif()
    set_tests_properties(permissions-core-tests permission-domain-tests
        generic-session-tests PROPERTIES
        ENVIRONMENT HOME=${KEYSHARP_DESKTOP_TEST_HOME})

    add_test(NAME version-command COMMAND keysharp-desktop version)
    set_tests_properties(version-command PROPERTIES
        PASS_REGULAR_EXPRESSION "^keysharp-desktop 0.2.0")
    add_test(NAME info-command COMMAND keysharp-desktop info)
    set_tests_properties(info-command PROPERTIES
        PASS_REGULAR_EXPRESSION
            "client_abi_minor=${KEYSHARP_DESKTOP_ABI_MINOR}")
    add_test(NAME unknown-command-rejected
        COMMAND keysharp-desktop unknown-command)
    set_tests_properties(unknown-command-rejected PROPERTIES WILL_FAIL TRUE)

    add_test(NAME bare-revoke-rejected COMMAND keysharp-desktop revoke)
    set_tests_properties(bare-revoke-rejected PROPERTIES WILL_FAIL TRUE)

    add_test(NAME provider-gate-test
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/provider_gate_test.cmake)

    add_test(NAME provider-generation-test
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/provider-generation-test
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/provider_generation_test.cmake)

    add_test(NAME release-layout-test
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/release_layout_test.cmake)

    add_test(NAME provider-syntax-test
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/provider_syntax_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(provider-syntax-test PROPERTIES SKIP_RETURN_CODE 77)

    add_test(NAME portable-compatibility-tests
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/portable_compatibility_tests.sh
            ${CMAKE_CURRENT_SOURCE_DIR})

    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        foreach(test_target
                protocol-tests
                provider-xml-tests
                request-chunk-tests
                authority-fd-tests
                client-abi-tests
                client-stream-tests
                capture-pipe-security-tests
                operation-scope-tests
                backend-session-tests
                generic-session-tests
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
set(CPACK_DEBIAN_PACKAGE_PROVIDES "keysharp-desktop-client-abi-0 (= 0.${KEYSHARP_DESKTOP_ABI_MINOR})")
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
