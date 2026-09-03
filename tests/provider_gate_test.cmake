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
            SetClipboardContent
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
        "const PUBLIC_IFACE_XML =[ \t\r\n]*`[^`]*"
        public_iface "${source}")
    if(NOT public_iface)
        message(FATAL_ERROR
            "${provider} public interface declaration not found")
    endif()
    foreach(forbidden GetWindowList CaptureArea GetClipboard SetClipboard)
        string(FIND "${public_iface}" "${forbidden}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR
                "${provider} public interface exposes ${forbidden}")
        endif()
    endforeach()
    string(REGEX MATCH
        "<(method|signal|property) name=\"[A-Za-z0-9_]*Capture[A-Za-z0-9_]*\""
        public_capture_bypass "${public_iface}")
    if(public_capture_bypass)
        message(FATAL_ERROR
            "${provider} public interface exposes capture: "
            "${public_capture_bypass}")
    endif()
    string(REGEX MATCH
        "<(method|signal|property) name=\"[A-Za-z0-9_]*Window[A-Za-z0-9_]*\""
        public_window_bypass "${public_iface}")
    if(public_window_bypass)
        message(FATAL_ERROR
            "${provider} public interface exposes sensitive window methods: "
            "${public_window_bypass}")
    endif()
    string(REGEX MATCH
        "<(method|signal|property) name=\"[A-Za-z0-9_]*Clipboard[A-Za-z0-9_]*\""
        public_clipboard_bypass "${public_iface}")
    if(public_clipboard_bypass)
        message(FATAL_ERROR
            "${provider} public interface exposes the clipboard: "
            "${public_clipboard_bypass}")
    endif()
    foreach(forbidden
            "SetClipboardText"
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
foreach(required
        "CaptureAreaAsync"
        "CaptureWindowAsync"
        "Gio.MemoryOutputStream.new_resizable"
        "paint_to_content(null)"
        "Shell.Screenshot.composite_to_stream"
        "_captureWindowRect"
        "_fitCaptureRect")
    string(FIND "${gnome_provider}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "GNOME in-memory capture invariant missing: ${required}")
    endif()
endforeach()
foreach(forbidden
        "\n    CaptureArea("
        "\n    CaptureWindow(")
    string(FIND "${gnome_provider}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR
            "GNOME exposes unguarded sync capture: ${forbidden}")
    endif()
endforeach()
file(READ "${SOURCE_DIR}/providers/cinnamon/extension.js" cinnamon_provider)
string(FIND "${cinnamon_provider}" "CaptureAreaAsync" cinnamon_area)
string(FIND "${cinnamon_provider}" "CaptureWindowAsync" cinnamon_window)
if(NOT cinnamon_area EQUAL -1 OR cinnamon_window EQUAL -1)
    message(FATAL_ERROR
        "Cinnamon must expose only its bounded in-memory window capture")
endif()
string(FIND "${cinnamon_provider}" "\n    CaptureWindow(" cinnamon_sync)
if(NOT cinnamon_sync EQUAL -1)
    message(FATAL_ERROR
        "cinnamon exposes unguarded sync method: CaptureWindow")
endif()
foreach(required
        "imports.gi.versions.Gdk = '3.0'"
        "actor.get_image"
        "_validCaptureGeometry(width, height)"
        "typeof pixbuf.save_to_streamv_async !== 'function'"
        "pixbuf.save_to_streamv_async(stream, 'png', [], [], null,"
        "GdkPixbuf.Pixbuf.save_to_stream_finish(result)"
        "MAX_CAPTURE_BYTES"
        "MAX_CAPTURE_DIMENSION"
        "MAX_CAPTURE_PIXELS")
    string(FIND "${cinnamon_provider}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "Cinnamon window capture invariant missing: ${required}")
    endif()
endforeach()
foreach(forbidden
        "Cinnamon.Screenshot"
        "screenshot_window("
        "\nconst Gdk =")
    string(FIND "${cinnamon_provider}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR
            "Cinnamon capture is not in-memory and lazily bound: ${forbidden}")
    endif()
endforeach()

file(READ "${SOURCE_DIR}/src/authorityd.c" authority)
foreach(required
        "KSD_DESKTOP_MANAGED_SCOPES"
        "KSD_DESKTOP_ACCEPTED_SCOPES"
        "store_config.read_scopes"
        "ksd_operation_scope(request->opcode)"
        "ksd_operation_scope_free(request->opcode)"
        "ksp_identity_revalidate"
        "ksp_store_check_at_generation"
        "KSD_MAX_AUTHORITY_WORKERS"
        "KSD_MAX_BACKEND_REGISTRATIONS"
        "ksd_backend_registration_magic"
        "registered_backend"
        "KSD_SYSTEM_SOCKET"
        "backend <= KSD_BACKEND_GENERIC"
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

file(READ "${SOURCE_DIR}/src/provider.c" provider_source)
foreach(required
        "g_timeout_source_new(KSD_PROVIDER_WATCH_POLL_MS)"
        "g_source_attach(timer, context)"
        "g_main_context_iteration(context, TRUE)")
    string(FIND "${provider_source}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "provider watch loop is not bounded-blocking: ${required}")
    endif()
endforeach()
foreach(forbidden
        "g_usleep"
        "g_main_context_iteration(context, FALSE)")
    string(FIND "${provider_source}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR
            "provider watch loop busy-polls: ${forbidden}")
    endif()
endforeach()

file(READ "${SOURCE_DIR}/src/operation_scope.c" operation_scope)
foreach(required
        "KSP_SCOPE_SCREEN_CAPTURE"
        "KSP_SCOPE_WINDOW_MONITORING"
        "KSP_SCOPE_WINDOW_CONTROL"
        "KSP_SCOPE_CLIPBOARD_MONITORING"
        "KSP_SCOPE_INPUT_CONTROL"
        "ksd_operation_scope_free")
    string(FIND "${operation_scope}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "operation scope mapping is missing: ${required}")
    endif()
endforeach()

file(READ "${SOURCE_DIR}/src/session_backend.c" session_backend)
foreach(required
        "SOCK_NONBLOCK"
        "errno != EINPROGRESS"
        "wait_for(descriptor, POLLOUT, deadline)"
        "SO_ERROR"
        "connect_backend(deadline)"
        "ksd_backend_session_unsupported()"
        "backend = KSD_BACKEND_GENERIC;"
        "register_backend(descriptor, backend, deadline)")
    string(FIND "${session_backend}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "session backend deadline invariant missing: ${required}")
    endif()
endforeach()

file(READ "${SOURCE_DIR}/src/backend.c" backend_selection)
foreach(required
        "kwin_wayland_owner()"
        "ksd_backend_session_unsupported"
        "backend == KSD_BACKEND_GENERIC"
        "strcmp(basename, \"kwin_wayland\") == 0")
    string(FIND "${backend_selection}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "KWin backend selection is not Wayland-gated: ${required}")
    endif()
endforeach()
file(READ "${SOURCE_DIR}/src/local_capture.c" local_capture)
string(FIND "${local_capture}" "strcmp(basename, \"kwin_wayland\") == 0"
    capture_executable_gate)
if(capture_executable_gate EQUAL -1)
    message(FATAL_ERROR
        "KWin capture no longer pins the kwin_wayland executable")
endif()

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

file(READ "${SOURCE_DIR}/data/keysharp-desktop.service.in" broker)
foreach(required
        "ProtectSystem=strict"
        "ProtectHome=read-only")
    string(FIND "${broker}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "compositor resources must stay read-only to the daemon: ${required}")
    endif()
endforeach()

file(READ "${SOURCE_DIR}/src/authorityd.c" authority_source)
foreach(required
        "ksd_request_chunk_admissible"
        "(uint32_t)KSD_MAX_REQUEST_ASSEMBLY_SECONDS"
        "set_socket_timeouts(session->descriptor, 130u)")
    string(FIND "${authority_source}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "authority request-assembly invariant missing: ${required}")
    endif()
endforeach()

set(assembly_end "end_assembly(session)")
string(LENGTH "${assembly_end}" assembly_end_length)
string(LENGTH "${authority_source}" assembly_full_length)
string(REPLACE "${assembly_end}" "" assembly_stripped "${authority_source}")
string(LENGTH "${assembly_stripped}" assembly_stripped_length)
math(EXPR assembly_end_count
    "(${assembly_full_length} - ${assembly_stripped_length}) / ${assembly_end_length}")
if(NOT assembly_end_count EQUAL 2)
    message(FATAL_ERROR
        "authorityd.c must end a request assembly on both exit paths")
endif()
