# "App" component: application layer (mode management, data acquisition, audio
# processing pipeline, light control, calibration, host communication, main).
file(GLOB_RECURSE APP_SRCS "${CMAKE_CURRENT_LIST_DIR}/*.c")

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS
        .
        mode_manager
        data_acquisition
        audio_processing
        light_control
        software_communication
        calibration
    REQUIRES Interface
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
