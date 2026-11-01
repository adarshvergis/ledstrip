# "Interface" component: all reusable submodules (drivers, DSP, storage, network, ...).
# Each submodule lives in its own sub-folder; every .c file found is compiled.
file(GLOB_RECURSE IFACE_SRCS "${CMAKE_CURRENT_LIST_DIR}/*.c")

idf_component_register(
    SRCS ${IFACE_SRCS}
    INCLUDE_DIRS
        common
        led_strip
        microphone
        signal_processing
        led_patterns
        config_manager
        network
        firmware_updater
        self_diagnostics
        cli
        error_handler
    REQUIRES
        esp_hw_support soc hal esp_rom esp_system esp_timer log heap
        esp_partition spi_flash app_update
        esp_wifi esp_netif esp_event lwip nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
