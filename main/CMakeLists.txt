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
        "web_api.c"
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
        esp_event
        esp_netif
        esp_http_server
        esp_http_client
        esp_https_ota
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
    button.c relay.c sensors.c power_meter.c ade7953.c chip_temp.c hw_config.c ota.c web_api.c status_led.c script_engine.c
    PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter"
)
