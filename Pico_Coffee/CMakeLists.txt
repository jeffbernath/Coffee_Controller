cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

set(PICO_BOARD pico CACHE STRING "Board type")

include(pico_sdk_import.cmake)
project(Pico_Coffee C CXX ASM)
pico_sdk_init()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(IO_MANIFEST ${CMAKE_CURRENT_LIST_DIR}/../config/io_manifest.json)
set(IO_GENERATOR ${CMAKE_CURRENT_LIST_DIR}/../tools/generate_io.py)
set(IO_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(IO_GENERATED_HEADER ${IO_GENERATED_DIR}/io_manifest.h)

add_custom_command(
    OUTPUT ${IO_GENERATED_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${IO_GENERATED_DIR}
    COMMAND ${Python3_EXECUTABLE} ${IO_GENERATOR} ${IO_MANIFEST} ${IO_GENERATED_HEADER}
    DEPENDS ${IO_MANIFEST} ${IO_GENERATOR}
    COMMENT "Generating Pico I/O definitions from config/io_manifest.json"
    VERBATIM
)
add_custom_target(generate_io_manifest DEPENDS ${IO_GENERATED_HEADER})

add_executable(Pico_Coffee
    Pico_Coffee.cpp
    communication.cpp
    io_runtime.cpp
    load_cells.cpp
    max31865.cpp
    ${IO_GENERATED_HEADER}
)
add_dependencies(Pico_Coffee generate_io_manifest)

pico_set_program_name(Pico_Coffee "Pico_Coffee")
pico_set_program_version(Pico_Coffee "0.3.0")

pico_enable_stdio_uart(Pico_Coffee 0)
pico_enable_stdio_usb(Pico_Coffee 1)

target_include_directories(Pico_Coffee PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${IO_GENERATED_DIR}
)

target_link_libraries(Pico_Coffee
    pico_stdlib
    pico_flash
    hardware_flash
    hardware_pwm
    hardware_clocks
    hardware_spi
)

pico_add_extra_outputs(Pico_Coffee)
