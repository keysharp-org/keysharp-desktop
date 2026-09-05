if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "SOURCE_DIR and OUTPUT_DIR are required")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${SOURCE_DIR}
        -DOUTPUT_DIR=${OUTPUT_DIR}
        -P ${SOURCE_DIR}/providers/shared/generate.cmake
    RESULT_VARIABLE generate_result)
if(NOT generate_result EQUAL 0)
    message(FATAL_ERROR "provider extension generation failed")
endif()

foreach(provider gnome cinnamon)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E compare_files
            ${OUTPUT_DIR}/${provider}/extension.js
            ${SOURCE_DIR}/providers/${provider}/extension.js
        RESULT_VARIABLE compare_result)
    if(NOT compare_result EQUAL 0)
        message(FATAL_ERROR
            "providers/${provider}/extension.js is stale; run the "
            "regenerate-provider-extensions target")
    endif()
endforeach()
