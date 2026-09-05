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
file(READ "${SOURCE_DIR}/nix/module.nix" nix_module)

execute_process(
    COMMAND sh "${SOURCE_DIR}/tests/installer_abi_tests.sh" "${SOURCE_DIR}"
    RESULT_VARIABLE installer_abi_result)
if(NOT installer_abi_result EQUAL 0)
    message(FATAL_ERROR "portable installer ABI contract check failed")
endif()

foreach(required
        "--skip-if-compatible"
        "payload/usr/local"
        "payload/usr/share/polkit-1/actions/org.keysharp.desktop.policy"
        "/usr/local/bin/keysharp-desktop"
        "/usr/local/libexec/keysharp-desktop-capture-worker"
        "has_required_resources"
        "abi_minor"
        "resource_configuration_matches"
        "installed_resources_are_protected"
        "is_root_private_executable"
        "policy_configuration_matches"
        "tmpfiles_configuration_matches"
        "desktop_entry_configuration_matches"
        "global_user_unit_available"
        "global_user_unit_enabled"
        "global_user_service_exec_matches"
        "systemd_socket_matches"
        "global_user_service_exec_matches keysharp-desktop.service \"$resolved\" daemon"
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
foreach(required
        "ListenStream = \"/run/keysharp-desktop/keysharp-desktop.sock\""
        "FileDescriptorName = \"public\"")
    string(FIND "${nix_module}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Nix socket is missing ${required}")
    endif()
endforeach()
set(nix_writable "ReadWritePaths")
string(LENGTH "${nix_writable}" nix_writable_length)
string(LENGTH "${nix_module}" nix_module_length)
string(REPLACE "${nix_writable}" "" nix_module_stripped "${nix_module}")
string(LENGTH "${nix_module_stripped}" nix_module_stripped_length)
math(EXPR nix_writable_count
    "(${nix_module_length} - ${nix_module_stripped_length}) / ${nix_writable_length}")
if(NOT nix_writable_count EQUAL 1)
    message(FATAL_ERROR
        "only the Nix authority service may declare ReadWritePaths")
endif()
string(FIND "${nix_module}" "systemd.user.services.keysharp-desktop = {"
    nix_user_start)
if(nix_user_start EQUAL -1)
    message(FATAL_ERROR "Nix module is missing the user service")
endif()
string(SUBSTRING "${nix_module}" ${nix_user_start} -1 nix_user_service)
foreach(required
        "PrivateTmp = false;"
        "ProtectSystem = false;"
        "ProtectHome = false;")
    string(FIND "${nix_user_service}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Nix user service is missing ${required}")
    endif()
endforeach()
foreach(forbidden
        "ReadWritePaths"
        "RuntimeDirectory"
        "StateDirectory"
        "BindPaths")
    string(FIND "${nix_user_service}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR
            "Nix user service must stay read-only; ${forbidden} is not allowed")
    endif()
endforeach()
foreach(forbidden
        "RuntimeDirectory"
        "StateDirectory"
        "BindPaths")
    string(FIND "${nix_module}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR
            "Nix module must not open a writable namespace through ${forbidden}")
    endif()
endforeach()
foreach(installer_source release_installer source_installer debian_postinst)
    foreach(required
            "refresh_invoking_user_manager"
            "SUDO_UID"
            "/usr/sbin/runuser"
            "/usr/bin/systemctl --user --no-pager daemon-reload"
            "keysharp-desktop.service")
        string(FIND "${${installer_source}}" "${required}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "${installer_source} user-session activation is missing ${required}")
        endif()
    endforeach()
endforeach()
foreach(lifecycle_source
        release_installer source_installer debian_postinst
        portable_uninstaller debian_postrm)
    string(FIND "${${lifecycle_source}}" "/sbin/ldconfig" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "${lifecycle_source} does not refresh the loader cache")
    endif()
endforeach()
string(FIND "${broker_unit}" "/keysharp-desktop daemon" broker_mode)
if(broker_mode EQUAL -1)
    message(FATAL_ERROR "user service does not select the daemon mode")
endif()
string(FIND "${authority_unit}" "/keysharp-desktop authority" authority_mode)
if(authority_mode EQUAL -1)
    message(FATAL_ERROR "system service does not select the authority mode")
endif()
file(READ "${SOURCE_DIR}/data/keysharp-desktop-authority.socket" authority_socket)
foreach(required
        "PartOf=graphical-session.target"
        "WantedBy=graphical-session.target"
        "Restart=on-failure"
        "NoNewPrivileges=true"
        "PrivateTmp=false"
        "ProtectSystem=false"
        "ProtectHome=false"
        "RestrictAddressFamilies=AF_UNIX"
        "LockPersonality=true")
    string(FIND "${broker_unit}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "user service is missing ${required}")
    endif()
endforeach()
foreach(forbidden
        "ReadWritePaths"
        "RuntimeDirectory"
        "StateDirectory"
        "BindPaths"
        "%t")
    string(FIND "${broker_unit}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR
            "user service must stay read-only; ${forbidden} is not allowed")
    endif()
endforeach()
foreach(required
        "ListenStream=/run/keysharp-desktop/keysharp-desktop.sock"
        "FileDescriptorName=public"
        "Accept=no"
        "SocketMode=0666"
        "DirectoryMode=0755")
    string(FIND "${authority_socket}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "authority socket is missing ${required}")
    endif()
endforeach()
foreach(required
        "NoNewPrivileges=true"
        "AmbientCapabilities=CAP_SETUID")
    string(FIND "${authority_unit}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "authority service is missing ${required}")
    endif()
endforeach()
foreach(required
        "NoNewPrivileges = true;"
        "AmbientCapabilities = [ \"CAP_SETUID\" ];")
    string(FIND "${nix_module}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Nix authority service is missing ${required}")
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
        "nix flake check --no-write-lock-file"
        "uninstall.sh")
    string(FIND "${release_workflow}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "release workflow is missing ${required}")
    endif()
endforeach()
foreach(workflow_source ci_workflow release_workflow)
    foreach(required
            "b8f31942dd2c286608d390634a9916bffce55ddf"
            "git rev-parse HEAD:third_party/keysharp-permissions"
            "libkeysharp-desktop.so.0")
        string(FIND "${${workflow_source}}" "${required}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "${workflow_source} misses ${required}")
        endif()
    endforeach()
endforeach()
foreach(required
        "inputs.keysharp-permissions"
        "b8f31942dd2c286608d390634a9916bffce55ddf"
        "flake = false")
    file(READ "${SOURCE_DIR}/flake.nix" flake_source)
    string(FIND "${flake_source}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Nix flake is missing ${required}")
    endif()
endforeach()
string(FIND "${release_workflow}" "Release blocked: generate, review, and commit flake.lock" blocked_without_lock)
if(NOT blocked_without_lock EQUAL -1)
    message(FATAL_ERROR "portable and Debian releases must not require flake.lock")
endif()
foreach(required
        "cachix/install-nix-action@v31"
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
        "systemctl --global disable keysharp-desktop.service"
        "stop keysharp-desktop.service"
        "reload_invoking_user_manager"
        "Other logged-in users were not contacted"
        "/usr/local/bin/keysharp-desktop"
        "/usr/local/libexec/keysharp-desktop-capture-worker"
        "/usr/local/lib/libkeysharp-desktop.so"
        "/usr/local/lib/libkeysharp-desktop.so.0"
        "library_payload=$(portable_library_payload || true)"
        "rm -f -- \"$library_payload\""
        "/usr/local/include/keysharp_desktop/client.h"
        "/usr/local/lib/pkgconfig/keysharp-desktop.pc"
        "/usr/local/lib/cmake/KeysharpDesktop"
        "/var/lib/keysharp-permissions/v1 were retained")
    string(FIND "${portable_uninstaller}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "portable uninstaller is missing ${required}")
    endif()
endforeach()
foreach(required
        "previous_library=$(portable_library_payload || true)"
        "current_library=$(portable_library_payload)"
        "payload_library=$(library_payload_under \"$payload\" || true)"
        "atomic_install_file \"$payload_library\""
        "atomic_install_symlink \"$payload/lib/libkeysharp-desktop.so.0\""
        "atomic_install_file \"$payload/bin/keysharp-desktop\""
        "atomic_install_file \"$payload/libexec/keysharp-desktop-capture-worker\""
        "rm -f -- \"$previous_library\"")
    string(FIND "${release_installer}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "portable installer does not retire the prior ABI payload: ${required}")
    endif()
endforeach()
string(FIND "${release_installer}" "cp -R \"$payload/.\" /usr/local/" found)
if(NOT found EQUAL -1)
    message(FATAL_ERROR "portable installer overwrites live files in place")
endif()
foreach(forbidden
        "keysharp-desktop.socket"
        "keysharp_desktop/protocol.h")
    string(FIND "${portable_uninstaller}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "portable uninstaller retains obsolete ${forbidden}")
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
        "/usr/local/lib/libkeysharp-desktop.so*"
        "/usr/local/libexec/keysharp-desktop-capture-worker"
        "/usr/local/lib/systemd/user/keysharp-desktop.service"
        "/usr/local/share/systemd/user/keysharp-desktop.service"
        "/usr/local/lib/systemd/system/keysharp-desktop-authority.service"
        "/usr/local/include/keysharp_desktop/client.h"
        "/usr/local/lib/pkgconfig/keysharp-desktop.pc"
        "/usr/local/lib/cmake/KeysharpDesktop"
        "/usr/local/lib/tmpfiles.d/keysharp-desktop-permissions.conf"
        "/usr/local/share/applications/org.keysharp.DesktopCapture.desktop"
        "/usr/local/share/gnome-shell/extensions/keysharp@keysharp.io"
        "/usr/local/share/cinnamon/extensions/keysharp@keysharp.io"
        "/usr/share/polkit-1/actions/org.keysharp.desktop.policy"
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
        "CPACK_DEBIAN_PACKAGE_PROVIDES \"keysharp-desktop-client-abi-0 (= 0."
        "CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON"
        "16986957+Descolada@users.noreply.github.com")
    string(FIND "${cmake_source}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Debian ownership transition is missing ${required}")
    endif()
endforeach()
