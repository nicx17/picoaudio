# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)

# Pull in SDK (must be before project())
include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(bluetooth_audio_receiver C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the SDK (must be after project())
pico_sdk_init()

add_executable(bluetooth_audio_receiver
    main.c
    bt_audio.c
    i2s_output.c
    led_status.c
    button_handler.c
)

# I2S pin assignments for the CJMCU-1334 DAC
# GPIO 16 = BCLK (clock_pin_base)
# GPIO 17 = WSEL/LRCLK (clock_pin_base + 1, automatic)
# GPIO 18 = DIN (data_pin)
target_compile_definitions(bluetooth_audio_receiver PRIVATE
    # I2S pin configuration
    PICO_AUDIO_I2S_DATA_PIN=18
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=16

    # Use PIO0 for I2S (PIO1 is available for future use)
    PICO_AUDIO_I2S_PIO=0

    # Stereo output
    PICO_AUDIO_I2S_MONO_OUTPUT=0
)

target_include_directories(bluetooth_audio_receiver PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(bluetooth_audio_receiver
    pico_stdlib
    pico_cyw43_arch_none
    pico_btstack_classic
    pico_btstack_cyw43
    pico_btstack_sbc_decoder
    pico_audio_i2s
    hardware_watchdog
    pico_multicore
)

# Enable USB output for debug logging, disable UART
pico_enable_stdio_usb(bluetooth_audio_receiver 1)
pico_enable_stdio_uart(bluetooth_audio_receiver 0)

# Generate UF2 output for easy flashing
pico_add_extra_outputs(bluetooth_audio_receiver)
