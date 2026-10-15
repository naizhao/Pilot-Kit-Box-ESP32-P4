# Keep this overlay local to the application build; never patch a shared SDK.
get_filename_component(_PK_DISPLAY_DMA_SYNC_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

function(pk_enable_display_dma_sync)
    idf_component_get_property(lcd_target esp_lcd COMPONENT_LIB)
    idf_component_get_property(lcd_directory esp_lcd COMPONENT_DIR)
    idf_build_get_property(build_directory BUILD_DIR)
    idf_build_get_property(python PYTHON)

    set(original "${lcd_directory}/dsi/esp_lcd_panel_dpi.c")
    set(generator "${_PK_DISPLAY_DMA_SYNC_ROOT}/scripts/prepare_display_dma_sync.py")
    # Preserve the object basename used by ESP-IDF's esp_lcd/linker.lf.
    set(overlay "${build_directory}/display_dma_sync/esp_lcd_panel_dpi.c")
    execute_process(
        COMMAND "${python}" "${generator}" --source "${original}" --output "${overlay}"
        RESULT_VARIABLE result
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Could not prepare the project DPI DMA overlay: ${error}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}" "${generator}")

    get_target_property(sources "${lcd_target}" SOURCES)
    set(updated_sources)
    set(matches 0)
    foreach(source IN LISTS sources)
        get_filename_component(absolute "${source}" ABSOLUTE BASE_DIR "${lcd_directory}")
        if(absolute STREQUAL original)
            list(APPEND updated_sources "${overlay}")
            math(EXPR matches "${matches} + 1")
        else()
            list(APPEND updated_sources "${source}")
        endif()
    endforeach()
    if(NOT matches EQUAL 1)
        message(FATAL_ERROR "Expected exactly one esp_lcd DPI translation unit, found ${matches}")
    endif()
    set_property(TARGET "${lcd_target}" PROPERTY SOURCES "${updated_sources}")
    target_include_directories("${lcd_target}" PRIVATE
        "${lcd_directory}/dsi"
        "${_PK_DISPLAY_DMA_SYNC_ROOT}/main"
    )
endfunction()
