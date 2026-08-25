idf_component_register(
    SRCS
        "app_main.cpp"
        "matter_device.cpp"
        "button.c"
        "relay.c"
        "sensors.c"
        "power_meter.c"
        "ade7953.c"
        "chip_temp.c"
        "hw_config.c"
        "ota.c"
        "shelly_boot.c"
        "loader_migrate.c"
        "stock_restore.c"
        "stock_fw.c"
        "web_api.c"
        "log_buffer.c"
        "status_led.c"
        "script_engine.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        lua
        json
        driver
        esp_timer
        esp_wifi
        esp_coex
        esp_event
        esp_netif
        esp_http_server
        esp_http_client
        esp_https_ota
        esp-tls
        app_update
        bt
        openthread
        espressif__mdns
        chip
        esp_matter
)

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

# C-modules zonder C++ extensies
set_source_files_properties(
    button.c relay.c sensors.c power_meter.c ade7953.c chip_temp.c hw_config.c ota.c shelly_boot.c loader_migrate.c stock_restore.c stock_fw.c web_api.c log_buffer.c status_led.c script_engine.c
    PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter"
)

# Embed the freshly built ESP-IDF bootloader into the app so loader_migrate.c
# can flash it to 0x0 during the one-time stock->ESP-IDF loader migration.
# Generated at build time (DEPENDS bootloader) so the .bin already exists.
idf_build_get_property(build_dir BUILD_DIR)
set(_embed_bl_bin "${build_dir}/bootloader/bootloader.bin")
set(_embed_bl_src "${CMAKE_CURRENT_BINARY_DIR}/embedded_bootloader.c")
add_custom_command(
    OUTPUT "${_embed_bl_src}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${_embed_bl_bin}" "-DOUTPUT=${_embed_bl_src}"
            -P "${CMAKE_CURRENT_LIST_DIR}/embed_bootloader.cmake"
    DEPENDS bootloader
    VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE "${_embed_bl_src}")
add_dependencies(${COMPONENT_LIB} bootloader)
