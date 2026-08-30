file(READ "${SOURCE_DIR}/packaging/install-release.sh" release_installer)
file(READ "${SOURCE_DIR}/install.sh" source_installer)
file(READ "${SOURCE_DIR}/packaging/debian/postinst" debian_postinst)
file(READ "${SOURCE_DIR}/packaging/debian/preinst" debian_preinst)
file(READ "${SOURCE_DIR}/uninstall.sh" portable_uninstaller)
file(READ "${SOURCE_DIR}/.github/workflows/release.yml" release_workflow)
file(READ "${SOURCE_DIR}/.github/workflows/ci.yml" ci_workflow)
file(READ "${SOURCE_DIR}/CMakeLists.txt" cmake_source)
file(READ "${SOURCE_DIR}/data/keysharp-desktop-authority.service.in" authority_unit)
file(READ "${SOURCE_DIR}/data/keysharp-desktop.service.in" broker_unit)
file(READ "${SOURCE_DIR}/packaging/debian/postrm" debian_postrm)

foreach(required
        "--skip-if-compatible"
        "protocol_version_compatible"
        "payload/usr/local"
        "payload/usr/share/polkit-1/actions/org.keysharp.desktop.policy"
        "/usr/local/bin/keysharp-desktop"
        "has_required_resources"
        "resource_configuration_matches"
        "installed_resources_are_protected"
        "policy_configuration_matches"
        "tmpfiles_configuration_matches"
        "desktop_entry_configuration_matches"
        "global_user_unit_available"
        "global_user_unit_enabled"
        "global_user_service_exec_matches"
        "global_user_socket_matches"
        "systemd_socket_matches"
        "global_user_service_exec_matches keysharp-desktop.service \"$resolved\" serve"
        "global_user_socket_matches keysharp-desktop.socket"
        "systemd_socket_matches keysharp-desktop-authority.socket"
        "/nix/store/*"
        "/run/current-system/sw/share/polkit-1/actions/org.keysharp.desktop.policy"
        "/usr/share/polkit-1/actions/org.keysharp.desktop.policy"
        "share/applications/org.keysharp.DesktopCapture.desktop"
        "share/gnome-shell/extensions/keysharp@keysharp.io/extension.js"
        "share/cinnamon/extensions/keysharp@keysharp.io/extension.js"
        "archive_root/uninstall.sh")
    string(FIND "${release_installer}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "portable release installer is missing ${required}")
    endif()
endforeach()
foreach(installer_source release_installer source_installer debian_postinst)
    foreach(required
            "refresh_invoking_user_manager"
            "SUDO_UID"
            "/usr/sbin/runuser"
            "/usr/bin/systemctl --user --no-pager daemon-reload"
            "/usr/bin/systemctl --user --no-pager try-restart"
            "/usr/bin/systemctl --user --no-pager start keysharp-desktop.socket")
        string(FIND "${${installer_source}}" "${required}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "${installer_source} user-session activation is missing ${required}")
        endif()
    endforeach()
endforeach()
string(FIND "${broker_unit}" "/keysharp-desktop serve" broker_mode)
if(broker_mode EQUAL -1)
    message(FATAL_ERROR "user service does not select the serve mode")
endif()
string(FIND "${authority_unit}" "/keysharp-desktop authority" authority_mode)
if(authority_mode EQUAL -1)
    message(FATAL_ERROR "system service does not select the authority mode")
endif()
file(READ "${SOURCE_DIR}/data/keysharp-desktop.socket" broker_socket)
file(READ "${SOURCE_DIR}/data/keysharp-desktop-authority.socket" authority_socket)
foreach(required
        "ListenStream=%t/keysharp-desktop/keysharp-desktop.sock"
        "Accept=no"
        "SocketMode=0600"
        "DirectoryMode=0700")
    string(FIND "${broker_socket}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "user socket is missing ${required}")
    endif()
endforeach()
foreach(required
        "ListenStream=/run/keysharp-desktop/authority.sock"
        "Accept=no"
        "SocketMode=0666"
        "DirectoryMode=0755")
    string(FIND "${authority_socket}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "authority socket is missing ${required}")
    endif()
endforeach()
foreach(forbidden
        "StateDirectory=keysharp-permissions"
        "RuntimeDirectory=keysharp-permissions")
    string(FIND "${authority_unit}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "authority service must not claim shared state through ${forbidden}")
    endif()
endforeach()

foreach(required
        "/var/lib/keysharp-permissions"
        "/run/keysharp-permissions")
    string(FIND "${authority_unit}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "authority service cannot write shared state ${required}")
    endif()
endforeach()
foreach(forbidden
        "add_executable(keysharp-desktopd"
        "add_executable(keysharp-desktopctl"
        "add_executable(keysharp-desktop-authorityd"
        "install(DIRECTORY interfaces/")
    string(FIND "${cmake_source}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "installed layout still contains ${forbidden}")
    endif()
endforeach()
foreach(lifecycle_source portable_uninstaller debian_postrm)
    string(FIND "${${lifecycle_source}}" "rm -rf -- /var/lib/keysharp-permissions" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "${lifecycle_source} must not purge shared permissions")
    endif()
endforeach()

foreach(required
        "build-portable"
        "-DCMAKE_INSTALL_PREFIX=/usr/local"
        "build-deb"
        "-DCMAKE_INSTALL_PREFIX=/usr"
        "workflow_dispatch"
        "deb_name=\"keysharp-desktop_"
        "nix_enabled"
        "if: needs.prepare.outputs.nix_enabled == 'true'"
        "uninstall.sh")
    string(FIND "${release_workflow}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "release workflow is missing ${required}")
    endif()
endforeach()
string(FIND "${release_workflow}" "Release blocked: generate, review, and commit flake.lock" blocked_without_lock)
if(NOT blocked_without_lock EQUAL -1)
    message(FATAL_ERROR "portable and Debian releases must not require flake.lock")
endif()
foreach(required
        "Detect locked Nix packaging"
        "if: steps.nix_lock.outputs.present == 'true'"
        "nix flake check --no-write-lock-file")
    string(FIND "${ci_workflow}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "CI Nix validation is missing ${required}")
    endif()
endforeach()

foreach(required
        "CMAKE_INSTALL_PREFIX STREQUAL \"/usr/local\""
        "/usr/share/polkit-1/actions"
        "keysharp-desktop-permissions.conf"
        "CMAKE_INSTALL_INCLUDEDIR}/keysharp_desktop")
    string(FIND "${cmake_source}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "portable polkit layout is missing ${required}")
    endif()
endforeach()

foreach(required
        "systemctl --global disable keysharp-desktop.socket"
        "stop keysharp-desktop.service"
        "reload_invoking_user_manager"
        "Other logged-in users were not contacted")
    string(FIND "${portable_uninstaller}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "portable uninstaller is missing ${required}")
    endif()
endforeach()

foreach(required preinst prerm postrm)
    if(NOT EXISTS "${SOURCE_DIR}/packaging/debian/${required}")
        message(FATAL_ERROR "Debian controls are missing packaging/debian/${required}")
    endif()
endforeach()
file(READ "${SOURCE_DIR}/packaging/debian/prerm" debian_prerm)
foreach(required
        "SUDO_UID"
        "/usr/sbin/runuser"
        "stop keysharp-desktop.service"
        "/usr/bin/systemctl --user --no-pager daemon-reload"
        "other logged-in users were not contacted")
    string(FIND "${debian_prerm}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Debian prerm user-session cleanup is missing ${required}")
    endif()
endforeach()
foreach(required
        "SUDO_UID"
        "/usr/sbin/runuser"
        "/usr/bin/systemctl --user --no-pager daemon-reload")
    string(FIND "${debian_postrm}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Debian postrm user-session cleanup is missing ${required}")
    endif()
endforeach()
foreach(required
        "/usr/local/bin/keysharp-desktop"
        "/usr/local/lib/systemd/user/keysharp-desktop.service"
        "/usr/local/share/systemd/user/keysharp-desktop.service"
        "/usr/local/lib/systemd/system/keysharp-desktop-authority.service"
        "[ -L \"\$candidate\" ]"
        "readlink -m"
        "sudo /usr/local/share/doc/keysharp-desktop/uninstall.sh")
    string(FIND "${debian_preinst}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Debian preinst portable-layer guard is missing ${required}")
    endif()
endforeach()
string(FIND "${debian_prerm}" "\${1:-}" prerm_operation_guard)
if(prerm_operation_guard EQUAL -1)
    message(FATAL_ERROR "Debian prerm must distinguish removal from upgrade")
endif()
string(FIND "${cmake_source}" "foreach(script preinst postinst prerm postrm)" found)
if(found EQUAL -1)
    message(FATAL_ERROR "CMake package does not include all Debian lifecycle controls")
endif()
foreach(required
        "CPACK_DEBIAN_PACKAGE_BREAKS \"keysharp (<< 0.0.0.17)\""
        "CPACK_DEBIAN_PACKAGE_REPLACES \"keysharp (<< 0.0.0.17)\""
        "CPACK_DEBIAN_PACKAGE_PROVIDES \"keysharp-desktop-protocol-1.2\""
        "CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON"
        "16986957+Descolada@users.noreply.github.com")
    string(FIND "${cmake_source}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Debian ownership transition is missing ${required}")
    endif()
endforeach()
