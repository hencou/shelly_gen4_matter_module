cmake_minimum_required(VERSION 3.13)
cmake_policy(SET CMP0079 NEW)

# Vereist: export ESP_MATTER_PATH naar je esp-matter checkout
# (zie INSTALL_VSCODE_WINDOWS.md).
if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Please set ESP_MATTER_PATH to the path of esp-matter repo")
endif()

set(PROJECT_VER "1.5.4")
set(PROJECT_VER_NUMBER 154)

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/components"
    "${MATTER_SDK_PATH}/config/esp32/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(shelly_gen4_matter_module)

# CHIP_HAVE_CONFIG_H is cruciaal — anders mist platform target selectie
# (ChipDevicePlatformEvent typedef, ConnectivityManagerImpl static_cast).
# Volgt het pattern van esp-matter's eigen voorbeelden (examples/light_switch).
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)

# main/ op de globale include-path zodat CHIPProjectConfig.h door alle
# componenten gevonden wordt — inclusief de CHIP SDK (GN build).
# Zonder dit vindt #include <CHIPProjectConfig.h> in CHIPConfig.h ons
# bestand niet, en valt CHIPDeviceConfig.h terug op "TEST_VENDOR"/"TEST_PRODUCT".
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_SOURCE_DIR}/main" APPEND)

# libopenthread_br.a references mDNS symbols (mdns_query_async_new,
# mdns_service_add, etc.) but the openthread component does not declare
# mdns as a dependency. Explicitly link mdns into openthread so the
# linker resolves these symbols.
idf_component_get_property(ot_lib openthread COMPONENT_LIB)
idf_component_get_property(mdns_lib espressif__mdns COMPONENT_LIB)
target_link_libraries(${ot_lib} PUBLIC ${mdns_lib})
