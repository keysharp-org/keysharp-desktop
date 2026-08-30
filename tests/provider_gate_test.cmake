foreach(provider gnome cinnamon)
    set(path "${SOURCE_DIR}/providers/${provider}/extension.js")
    file(READ "${path}" source)
    foreach(required
            "GLib.path_get_basename(target) !== 'keysharp-desktop'"
            "uid === 0"
            "(mode & 0o022) === 0"
            "RegisterBrokerAsync"
            "GetWindowListAsync"
            "GetActiveWindowAsync"
            "GetClipboardMimetypesAsync"
            "GetClipboardContentAsync"
            "GetClipboardTextAsync"
            "FocusWindowAsync"
            "MoveResizeWindowAsync"
            "CloseWindowAsync"
            "_emitWindowEventRaw('active-state', this._getActiveWindow())"
            "_emitBrokerSignal('WindowEvent'"
            "_emitBrokerSignal('ClipboardChanged'")
        string(FIND "${source}" "${required}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR "${provider} provider is missing caller gate: ${required}")
        endif()
    endforeach()
    string(FIND "${source}" "mode & 0o4000" setuid_gate)
    if(NOT setuid_gate EQUAL -1)
        message(FATAL_ERROR "${provider} provider still requires a setuid broker")
    endif()
    foreach(sensitive_method
            RegisterBroker
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
            CaptureArea
            CaptureWindow)
        string(FIND "${source}" "\n    ${sensitive_method}(" sync_definition)
        if(NOT sync_definition EQUAL -1)
            message(FATAL_ERROR
                "${provider} provider exposes unguarded sync method: ${sensitive_method}")
        endif()
    endforeach()
    foreach(forbidden
            "this._dbusImpl.emit_signal('WindowEvent'"
            "this._dbusImpl.emit_signal('ClipboardChanged'")
        string(FIND "${source}" "${forbidden}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR "${provider} provider broadcasts sensitive signal: ${forbidden}")
        endif()
    endforeach()
endforeach()

file(READ "${SOURCE_DIR}/data/org.keysharp.desktop.policy" policy)
string(FIND "${policy}" "<message>$(polkit.message)</message>" dynamic_message)
if(dynamic_message EQUAL -1)
    message(FATAL_ERROR "desktop polkit policy must display the sanitized application and scope detail")
endif()

file(READ "${SOURCE_DIR}/data/keysharp-desktop-authority.service.in" authority_unit)
string(FIND "${authority_unit}" "RestrictAddressFamilies=AF_UNIX AF_NETLINK AF_ALG" af_alg)
if(af_alg EQUAL -1)
    message(FATAL_ERROR "authority systemd sandbox must allow AF_ALG for executable hashing")
endif()

file(READ "${SOURCE_DIR}/src/authorityd.c" authority_source)
foreach(required
        "KSD_MAX_AUTHORITY_WORKERS"
        "SO_RCVTIMEO"
        "pthread_create"
        "\"app.path\""
        "\"desktop.capability-names\""
        "\"polkit.message\"")
    string(FIND "${authority_source}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "authority security invariant missing: ${required}")
    endif()
endforeach()
