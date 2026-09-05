if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "SOURCE_DIR and OUTPUT_DIR are required")
endif()

file(READ "${SOURCE_DIR}/providers/shared/provider-core.js" provider_core)
file(READ
    "${SOURCE_DIR}/interfaces/private/org.keysharp.Desktop.Provider1.xml"
    provider_interface)
string(FIND "${provider_core}" "@PROVIDER_INTERFACE@" interface_marker)
if(interface_marker EQUAL -1)
    message(FATAL_ERROR "provider-core.js lacks the interface marker")
endif()
string(REPLACE "@PROVIDER_INTERFACE@" "${provider_interface}"
    provider_core "${provider_core}")

foreach(provider gnome cinnamon)
    file(READ "${SOURCE_DIR}/providers/shared/${provider}-shim.js" shim)
    string(FIND "${shim}" "@PROVIDER_CORE@" core_marker)
    if(core_marker EQUAL -1)
        message(FATAL_ERROR "${provider}-shim.js lacks the core marker")
    endif()
    string(REPLACE "@PROVIDER_CORE@" "${provider_core}"
        generated "${shim}")
    string(PREPEND generated
        "// Generated from providers/shared; edit and regenerate those sources.\n")
    file(MAKE_DIRECTORY "${OUTPUT_DIR}/${provider}")
    file(WRITE "${OUTPUT_DIR}/${provider}/extension.js" "${generated}")
endforeach()
