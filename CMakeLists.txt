cmake_minimum_required(VERSION 3.20)

project(keysharp-desktop
    VERSION 0.1.0
    DESCRIPTION "Per-application desktop capability broker for Linux"
    HOMEPAGE_URL "https://github.com/keysharp-org/keysharp-desktop"
    LANGUAGES C)

include(GNUInstallDirs)
include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)

add_library(keysharp-desktop-common STATIC src/common.c)
target_include_directories(keysharp-desktop-common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(keysharp-desktop-common PUBLIC _GNU_SOURCE _POSIX_C_SOURCE=200809L)

add_executable(keysharp-desktop
    src/main.c
    src/desktopd.c
    src/capture.c
    src/provider.c
    src/authority_client.c
    src/authorityd.c
    src/shared_grants.c
    src/polkit_result.c
    src/desktopctl.c)
target_link_libraries(keysharp-desktop PRIVATE
    keysharp-desktop-common PkgConfig::GIO Threads::Threads)
set(KSD_PKCHECK_PATH "/usr/bin/pkcheck" CACHE FILEPATH "absolute path to pkcheck")
target_compile_definitions(keysharp-desktop PRIVATE
    KSD_PKCHECK_PATH="${KSD_PKCHECK_PATH}")

foreach(target keysharp-desktop-common keysharp-desktop)
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

install(TARGETS keysharp-desktop RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/keysharp-desktop.service data/keysharp-desktop.socket
    DESTINATION ${KEYSHARP_DESKTOP_SYSTEMD_USER_DIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/keysharp-desktop-authority.service
              data/keysharp-desktop-authority.socket
    DESTINATION ${KEYSHARP_DESKTOP_SYSTEMD_SYSTEM_DIR})
install(FILES data/keysharp-desktop-permissions.conf DESTINATION lib/tmpfiles.d)
install(FILES data/org.keysharp.desktop.policy DESTINATION ${KEYSHARP_DESKTOP_POLKIT_ACTION_DIR})
install(FILES include/keysharp_desktop/protocol.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/keysharp_desktop)
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
    target_link_libraries(protocol-tests PRIVATE keysharp-desktop-common)
    add_test(NAME protocol-tests COMMAND protocol-tests)

    add_executable(authority-fd-tests tests/authority_fd_tests.c)
    target_link_libraries(authority-fd-tests PRIVATE keysharp-desktop-common)
    add_test(NAME authority-fd-tests COMMAND authority-fd-tests)

    add_executable(session-protocol-tests tests/session_protocol_tests.c)
    target_link_libraries(session-protocol-tests PRIVATE keysharp-desktop-common)
    add_test(NAME session-protocol-tests
        COMMAND session-protocol-tests $<TARGET_FILE:keysharp-desktop>)
    add_test(NAME version-command COMMAND keysharp-desktop version)
    set_tests_properties(version-command PROPERTIES
        PASS_REGULAR_EXPRESSION "capability_clipboard_monitoring=0x00000020"
        FAIL_REGULAR_EXPRESSION "accessibility|cursor")
    add_test(NAME info-alias COMMAND keysharp-desktop --info)
    set_tests_properties(info-alias PROPERTIES
        PASS_REGULAR_EXPRESSION "protocol_minor=2")
    add_test(NAME explicit-mode-required COMMAND keysharp-desktop)
    set_tests_properties(explicit-mode-required PROPERTIES WILL_FAIL TRUE)

    add_executable(keysharp-desktop-serve-revocation-test
        tests/serve_entry.c src/desktopd.c src/capture.c src/provider.c
        src/authority_client.c src/common.c)
    target_include_directories(keysharp-desktop-serve-revocation-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(keysharp-desktop-serve-revocation-test PRIVATE
        _GNU_SOURCE _POSIX_C_SOURCE=200809L
        KSD_AUTHORITY_SOCKET="/tmp/keysharp-desktop-session-revoke-authority.sock"
        KSD_RUNTIME_DIRECTORY="/tmp/keysharp-desktop-session-revoke-runtime"
        KSD_HANDSHAKE_TIMEOUT_SECONDS=1u
        KSD_REVOKE_GENERATION_OWNER_UID=getuid\(\))
    target_link_libraries(keysharp-desktop-serve-revocation-test PRIVATE PkgConfig::GIO Threads::Threads)

    add_executable(session-revocation-tests tests/session_revocation_tests.c src/common.c)
    target_include_directories(session-revocation-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(session-revocation-tests PRIVATE
        _GNU_SOURCE _POSIX_C_SOURCE=200809L
        KSD_RUNTIME_DIRECTORY="/tmp/keysharp-desktop-session-revoke-runtime"
        KSD_REVOKE_GENERATION_OWNER_UID=getuid\(\))
    add_test(NAME session-revocation-tests
        COMMAND session-revocation-tests $<TARGET_FILE:keysharp-desktop-serve-revocation-test>)

    add_executable(polkit-result-tests tests/polkit_result_tests.c src/polkit_result.c)
    target_include_directories(polkit-result-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME polkit-result-tests COMMAND polkit-result-tests)

    add_executable(revocation-tests tests/revocation_tests.c src/common.c src/shared_grants.c)
    target_include_directories(revocation-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(revocation-tests PRIVATE
        _GNU_SOURCE _POSIX_C_SOURCE=200809L
        KSD_GRANT_DIRECTORY="/tmp/keysharp-desktop-revocation-test-state"
        KSD_RUNTIME_DIRECTORY="/tmp/keysharp-desktop-revocation-test-runtime"
        KSD_REVOKE_GENERATION_OWNER_UID=getuid\(\))
    add_test(NAME revocation-tests COMMAND revocation-tests)

    add_executable(prompt-lock-tests
        tests/prompt_lock_tests.c src/common.c src/shared_grants.c)
    target_include_directories(prompt-lock-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(prompt-lock-tests PRIVATE
        _GNU_SOURCE _POSIX_C_SOURCE=200809L
        KSD_GRANT_DIRECTORY="/tmp/keysharp-desktop-prompt-lock-test-state"
        KSD_RUNTIME_DIRECTORY="/tmp/keysharp-desktop-prompt-lock-test-runtime"
        KSD_REVOKE_GENERATION_OWNER_UID=getuid\(\))
    add_test(NAME prompt-lock-tests COMMAND prompt-lock-tests)

    add_executable(grant-admin-tests
        tests/grant_admin_tests.c src/common.c src/shared_grants.c)
    target_include_directories(grant-admin-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(grant-admin-tests PRIVATE
        _GNU_SOURCE _POSIX_C_SOURCE=200809L
        KSD_GRANT_DIRECTORY="/tmp/keysharp-desktop-grant-admin-test-state"
        KSD_RUNTIME_DIRECTORY="/tmp/keysharp-desktop-grant-admin-test-runtime"
        KSD_REVOKE_GENERATION_OWNER_UID=getuid\(\))
    add_test(NAME grant-admin-tests COMMAND grant-admin-tests)

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
                session-protocol-tests
                keysharp-desktop-serve-revocation-test
                session-revocation-tests
                polkit-result-tests
                revocation-tests
                prompt-lock-tests
                grant-admin-tests)
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
set(CPACK_DEBIAN_PACKAGE_PROVIDES "keysharp-desktop-protocol-1.2")
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
