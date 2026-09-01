if(NOT EXISTS "${LIBRARY}" OR NOT EXISTS "${CLIENT_HEADER}" OR NOT NM)
    message(FATAL_ERROR "exported-symbol test inputs are unavailable")
endif()

execute_process(
    COMMAND "${NM}" -D --defined-only "${LIBRARY}"
    RESULT_VARIABLE nm_status
    OUTPUT_VARIABLE nm_output
    ERROR_VARIABLE nm_error)
if(NOT nm_status EQUAL 0)
    message(FATAL_ERROR "nm failed: ${nm_error}")
endif()

string(REGEX MATCHALL "[^\r\n]+" nm_lines "${nm_output}")
set(actual)
foreach(line IN LISTS nm_lines)
    string(REGEX MATCH "ksd_[A-Za-z0-9_]+$" symbol "${line}")
    if(symbol)
        list(APPEND actual "${symbol}")
    endif()
endforeach()

file(READ "${CLIENT_HEADER}" header)
string(REGEX MATCHALL "KSD_API[^;]+;" declarations "${header}")
set(expected)
foreach(declaration IN LISTS declarations)
    string(REGEX MATCH "ksd_[A-Za-z0-9_]+[ \t\r\n]*\\(" match "${declaration}")
    if(match)
        string(REGEX REPLACE "[ \t\r\n]*\\($" "" symbol "${match}")
        list(APPEND expected "${symbol}")
    endif()
endforeach()

list(SORT actual)
list(SORT expected)
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
        "client shared-library exports differ from client.h\n"
        "expected: ${expected}\nactual: ${actual}")
endif()
