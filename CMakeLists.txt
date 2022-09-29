cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Pull in Raspberry Pi Pico SDK (must be before project)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

set(CMAKE_BUILD_TYPE Debug)
set(PICO_BOARD pico_w)
set(PROJECT_NAME wifi_modem)
project(${PROJECT_NAME})
set(CYW43_HOST_NAME "PicoW2")

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(${PROJECT_NAME}
      ${PROJECT_NAME}.cpp
      )
target_compile_definitions(${PROJECT_NAME} PRIVATE
      CYW43_HOST_NAME=\"${CYW43_HOST_NAME}\"
      )
target_include_directories(${PROJECT_NAME} PRIVATE
      ${CMAKE_CURRENT_LIST_DIR}
      )
target_link_libraries(${PROJECT_NAME}
      pico_cyw43_arch_lwip_threadsafe_background
      pico_stdlib
      hardware_i2c
      )

pico_add_extra_outputs(${PROJECT_NAME})

# enable usb output, disable uart output
pico_enable_stdio_usb(${PROJECT_NAME} 0)
pico_enable_stdio_uart(${PROJECT_NAME} 1)
