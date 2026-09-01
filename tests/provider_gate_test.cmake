foreach(provider gnome cinnamon)
    set(path "${SOURCE_DIR}/providers/${provider}/extension.js")
    file(READ "${path}" source)

    foreach(required
            "org.keysharp.Desktop.Provider1"
            "PUBLIC_IFACE_XML"
            "Gio.DBusServer.new_sync"
            "allow-mechanism"
            "mechanism === 'EXTERNAL'"
            "authorize-authenticated-peer"
            "_credentialUid(credentials) === 0"
            "_credentialUid(connection.get_peer_credentials()) !== 0"
            "_requireProviderPeer(invocation)"
            "_callerIsProviderPeer(invocation)"
            "this._providerConnections.has(connection)"
            "SendMouseMoveAbsoluteAsync"
            "SendMouseMoveRelativeAsync"
            "SendMouseButtonAsync"
            "SendMouseScrollAsync")
        string(FIND "${source}" "${required}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR
                "${provider} provider security invariant missing: ${required}")
        endif()
    endforeach()

    foreach(sensitive_method
            GetWindowList
            GetActiveWindow
            GetClipboardMimetypes
            GetClipboardContent
            GetClipboardText
            FocusWindow
            RaiseWindow
            LowerWindow
            MoveResizeWindow
            MoveResizeWindowByXid
            SetWindowState
            SetWindowAbove
            SetWindowDecorated
            SetWindowOpacity
            CloseWindow
            KillWindow
            ReserveWindow
            GetReservedWindow
            SendMouseMoveAbsolute
            SendMouseMoveRelative
            SendMouseButton
            SendMouseScroll)
        string(FIND "${source}" "\n    ${sensitive_method}(" sync_definition)
        if(NOT sync_definition EQUAL -1)
            message(FATAL_ERROR
                "${provider} exposes unguarded sync method: ${sensitive_method}")
        endif()
        string(FIND "${source}" "${sensitive_method}Async" async_definition)
        if(async_definition EQUAL -1)
            message(FATAL_ERROR
                "${provider} lacks guarded async method: ${sensitive_method}")
        endif()
    endforeach()

    foreach(required
            "MAX_PLACEMENTS_TOTAL"
            "MAX_PLACEMENTS_PER_PID"
            "MAX_PLACED_TOTAL"
            "MAX_OVERLAY_BYTES_PER_OWNER"
            "MAX_OVERLAY_BYTES_TOTAL"
            "_overlayBytesAvailable")
        string(FIND "${source}" "${required}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR
                "${provider} provider placement quota missing: ${required}")
        endif()
    endforeach()
    foreach(forbidden
            "Gio.File.new_tmp"
            "writeToPNG(")
        string(FIND "${source}" "${forbidden}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR
                "${provider} provider writes capture pixels to a named file: ${forbidden}")
        endif()
    endforeach()

    string(REGEX MATCH
        "const PUBLIC_IFACE_XML =[^;]*GetWindowList"
        public_window_bypass "${source}")
    if(public_window_bypass)
        message(FATAL_ERROR
            "${provider} public interface exposes sensitive window methods")
    endif()
    string(REGEX MATCH
        "const PUBLIC_IFACE_XML =[^;]*CaptureArea"
        public_capture_bypass "${source}")
    if(public_capture_bypass)
        message(FATAL_ERROR
            "${provider} public interface exposes capture")
    endif()
    string(REGEX MATCH
        "const PUBLIC_IFACE_XML =[^;]*GetClipboard"
        public_clipboard_bypass "${source}")
    if(public_clipboard_bypass)
        message(FATAL_ERROR
            "${provider} public interface exposes clipboard reads")
    endif()
    foreach(forbidden
            "RegisterBroker"
            "_emitBrokerSignal"
            "this._dbusImpl.emit_signal('WindowEvent'"
            "this._dbusImpl.emit_signal('ClipboardChanged'")
        string(FIND "${source}" "${forbidden}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR
                "${provider} retains public broker bypass: ${forbidden}")
        endif()
    endforeach()
endforeach()

file(READ "${SOURCE_DIR}/providers/gnome/extension.js" gnome_provider)
string(FIND "${gnome_provider}" "CaptureAreaAsync" gnome_area)
string(FIND "${gnome_provider}" "CaptureWindowAsync" gnome_window)
if(gnome_area EQUAL -1 OR NOT gnome_window EQUAL -1)
    message(FATAL_ERROR
        "GNOME must expose only its bounded in-memory area capture")
endif()
file(READ "${SOURCE_DIR}/providers/cinnamon/extension.js" cinnamon_provider)
foreach(forbidden CaptureAreaAsync CaptureWindowAsync)
    string(FIND "${cinnamon_provider}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR
            "Cinnamon must not expose file-only capture: ${forbidden}")
    endif()
endforeach()

file(READ "${SOURCE_DIR}/src/authorityd.c" authority)
foreach(required
        "KSD_DESKTOP_MANAGED_SCOPES"
        "KSD_DESKTOP_ACCEPTED_SCOPES"
        "store_config.read_scopes"
        "KSP_SCOPE_INPUT_CONTROL"
        "ksp_identity_revalidate"
        "ksp_store_check_at_generation"
        "KSD_MAX_AUTHORITY_WORKERS"
        "KSD_MAX_BACKEND_REGISTRATIONS"
        "ksd_backend_registration_magic"
        "registered_backend"
        "KSD_SYSTEM_SOCKET"
        "ksd_capture_worker_execute")
    string(FIND "${authority}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "authority invariant missing: ${required}")
    endif()
endforeach()
foreach(forbidden
        "request_digest"
        "authority_protocol"
        "backend.sock"
        "authority.sock"
        "(void)setsockopt")
    string(FIND "${authority}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "authority retains obsolete path: ${forbidden}")
    endif()
endforeach()
foreach(required
        "set_socket_timeouts(connection, 5u)"
        "set_socket_timeouts(client->descriptor, 130u)")
    string(FIND "${authority}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "authority timeout failure is not fail-closed: ${required}")
    endif()
endforeach()

file(READ "${SOURCE_DIR}/src/session_backend.c" session_backend)
foreach(required
        "SOCK_NONBLOCK"
        "errno != EINPROGRESS"
        "wait_for(descriptor, POLLOUT, deadline)"
        "SO_ERROR"
        "connect_backend(deadline)"
        "register_backend(descriptor, backend, deadline)")
    string(FIND "${session_backend}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "session backend deadline invariant missing: ${required}")
    endif()
endforeach()

file(READ "${SOURCE_DIR}/src/permission_domain.h" domain)
foreach(required
        "KSP_SCOPE_INPUT_CONTROL | KSP_SCOPE_WINDOW_MONITORING"
        "KSD_DESKTOP_ACCEPTED_SCOPES KSD_DESKTOP_MANAGED_SCOPES")
    string(FIND "${domain}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "desktop permission domain is missing ${required}")
    endif()
endforeach()

file(READ "${SOURCE_DIR}/data/org.keysharp.desktop.policy" policy)
string(FIND "${policy}" "<message>$(polkit.message)</message>" dynamic_message)
if(dynamic_message EQUAL -1)
    message(FATAL_ERROR
        "desktop polkit policy must use the sanitized dynamic message")
endif()

file(READ "${SOURCE_DIR}/data/keysharp-desktop-authority.service.in" unit)
foreach(required
        "User=root"
        "RestrictAddressFamilies=AF_UNIX AF_NETLINK AF_ALG"
        "ReadWritePaths=/var/lib/keysharp-permissions")
    string(FIND "${unit}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "authority unit invariant missing: ${required}")
    endif()
endforeach()
