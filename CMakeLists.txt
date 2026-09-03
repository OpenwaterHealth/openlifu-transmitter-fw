cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# ---------------------------
# Version info: generated at build time via version.h
# ---------------------------

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Generate version.h at configure time (initial)
find_package(Git QUIET)

if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE FW_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE GIT_DESCRIBE_RESULT
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE FW_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE GIT_SHA_RESULT
    )
    if(NOT GIT_DESCRIBE_RESULT EQUAL 0)
        set(FW_GIT_DESCRIBE "unknown")
    endif()
    if(NOT GIT_SHA_RESULT EQUAL 0)
        set(FW_GIT_SHA "unknown")
    endif()
else()
    set(FW_GIT_DESCRIBE "unknown")
    set(FW_GIT_SHA "unknown")
endif()

string(TIMESTAMP FW_BUILD_TIME_UTC "%Y-%m-%d %H:%M:%S UTC" UTC)

message(STATUS "FW version   : ${FW_GIT_DESCRIBE}")
message(STATUS "FW git sha   : ${FW_GIT_SHA}")
message(STATUS "FW build time: ${FW_BUILD_TIME_UTC}")

configure_file(
    ${CMAKE_SOURCE_DIR}/version.h.in
    ${CMAKE_BINARY_DIR}/generated/version.h
    @ONLY
)

# Set the project name
set(CMAKE_PROJECT_NAME lifu-transmitter-fw)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Bare-metal build option (mirrors openlifu-console-fw):
#   BARE_METAL=OFF (default) : image runs under the SBSFU secure bootloader from
#                              the active slot — FLASH origin=0x08010400,
#                              LENGTH=187K, VTOR=0x08010400 (slot + signed-header
#                              offset; uses STM32L443XX_FLASH.ld). This is the
#                              image that gets signed.
#   BARE_METAL=ON            : standalone image at 0x08000000, LENGTH=256K, no
#                              bootloader (uses STM32L443RCIX_FLASH.ld).
option(BARE_METAL "Build a bare-metal image (no bootloader; runs at 0x08000000)" OFF)

if(BARE_METAL)
    message(STATUS "Bare-metal build : FLASH origin=0x08000000, LENGTH=256K")
    set(LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/STM32L443RCIX_FLASH.ld")
else()
    message(STATUS "Bootloader build : FLASH origin=0x08010400, LENGTH=187K, VTOR=0x08010400 (SBSFU slot + signed-header offset)")
    set(LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/STM32L443XX_FLASH.ld")
endif()

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Strip the CubeMX-hardcoded linker script so the root can control it conditionally
# (safe to re-run when CubeMX regenerates cmake/stm32cubemx/CMakeLists.txt)
string(REGEX REPLACE "-T[^ \"]*|\\-T \"[^\"]*\"" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Optional DMA-backed UART debug logging (off by default)
option(DEBUG_ENABLED "Enable DMA-backed UART debug logging" OFF)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    USB_DEVICE/App/usb_events.c
    Core/Src/lifu_config.c
    Core/Src/afe_config.c
    Core/Src/flash_eeprom.c
    Core/Src/i2c_protocol.c
    Core/Src/i2c_master.c
    Core/Src/i2c_slave.c
    Core/Src/if_commands.c
    Core/Src/jsmn.c
    Core/Src/lwrb.c    
    Core/Src/module_manager.c
    Core/Src/thermistor.c
    Core/Src/trigger.c
    Core/Src/tx7332.c
    Core/Src/uart_comms.c
    Core/Src/utils.c
    Core/Src/demo.c
    Core/Src/presets.c
)

# ---------------------------
# FDA mode
# ---------------------------
# FDA_MODE names a directory under presets/ whose *.json files are baked into
# the image. In this mode the firmware -- not the host -- programs the TX7332
# chips: the host lists the presets over OW_PRESET_GET and asks for one with
# OW_PRESET_LOAD, and host register writes are refused (see Core/Inc/presets.h).
# "OFF" bakes nothing and leaves the image and its behaviour unchanged.
#
# The .json files are the source of truth. Their register blocks are
# precomputed by tools/precompute_registers.py (which needs the openlifu SDK)
# and committed under presets/<set>/registers/; the build itself needs only the
# standard library. Drop a .json in, re-run precompute_registers.py, rebuild.
#
# The JSON is stored uncompressed and competes with code for the flash region
# the image links into: 187K in a bootloader build, 256K bare metal. The size
# report below says what the current set costs; if it does not fit, the link
# fails with a region overflow.
set(FDA_MODE "OFF" CACHE STRING "Preset set to bake in: a directory under presets/, or OFF")

if(NOT FDA_MODE STREQUAL "OFF" AND NOT FDA_MODE STREQUAL "none")
    set(PRESET_CONTEXT "${FDA_MODE}")
    set(PRESET_SRC_DIR "${CMAKE_SOURCE_DIR}/presets/${PRESET_CONTEXT}")
    set(PRESET_OUT_DIR "${CMAKE_BINARY_DIR}/generated/presets/${PRESET_CONTEXT}")

    if(NOT IS_DIRECTORY "${PRESET_SRC_DIR}")
        message(FATAL_ERROR "FDA_MODE=${FDA_MODE} but ${PRESET_SRC_DIR} does not exist")
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    # CONFIGURE_DEPENDS re-runs the glob when a .json is added or removed;
    # content changes are picked up by the custom command's DEPENDS below.
    file(GLOB PRESET_JSON_FILES CONFIGURE_DEPENDS "${PRESET_SRC_DIR}/*.json")
    if(NOT PRESET_JSON_FILES)
        message(FATAL_ERROR "No .json files in ${PRESET_SRC_DIR}")
    endif()

    list(LENGTH PRESET_JSON_FILES PRESET_FILE_COUNT)

    set(PRESET_GEN_ARGS --src "${PRESET_SRC_DIR}" --out "${PRESET_OUT_DIR}" --context "${PRESET_CONTEXT}")

    # Generate once at configure time so the headers exist for the first build
    # and for IDE indexing.
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_presets.py ${PRESET_GEN_ARGS}
        RESULT_VARIABLE PRESET_GEN_RESULT
        OUTPUT_VARIABLE PRESET_GEN_OUTPUT
        ERROR_VARIABLE PRESET_GEN_OUTPUT
    )
    if(NOT PRESET_GEN_RESULT EQUAL 0)
        message(FATAL_ERROR "gen_presets.py failed:\n${PRESET_GEN_OUTPUT}")
    endif()

    # Report what actually landed in flash, i.e. the minified total.
    if(PRESET_GEN_OUTPUT MATCHES "([0-9]+) bytes of JSON minified from ([0-9]+), ([0-9]+) TX7332")
        set(PRESET_TOTAL_BYTES ${CMAKE_MATCH_1})
        set(PRESET_RAW_BYTES ${CMAKE_MATCH_2})
        set(PRESET_REG_COUNT ${CMAKE_MATCH_3})
    else()
        set(PRESET_TOTAL_BYTES 0)
        set(PRESET_RAW_BYTES 0)
        set(PRESET_REG_COUNT 0)
    endif()
    math(EXPR PRESET_TOTAL_KB "${PRESET_TOTAL_BYTES} / 1024")

    # Regenerate during the build when a .json or the generator changes.
    add_custom_command(
        OUTPUT "${PRESET_OUT_DIR}/preset_table.h"
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_presets.py ${PRESET_GEN_ARGS}
        DEPENDS ${PRESET_JSON_FILES} "${CMAKE_SOURCE_DIR}/tools/gen_presets.py"
        COMMENT "Baking ${PRESET_FILE_COUNT} '${PRESET_CONTEXT}' preset(s) into headers"
        VERBATIM
    )
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE "${PRESET_OUT_DIR}/preset_table.h")

    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        PRESET_TABLE_HEADER="presets/${PRESET_CONTEXT}/preset_table.h")

    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE FDA_MODE)

    message(STATUS "FDA mode    : ${FDA_MODE}, ${PRESET_FILE_COUNT} preset(s), "
                   "${PRESET_TOTAL_BYTES} bytes (~${PRESET_TOTAL_KB} KB) of JSON "
                   "minified from ${PRESET_RAW_BYTES}, plus ${PRESET_REG_COUNT} TX7332 registers")
else()
    message(STATUS "FDA mode    : OFF (host owns the TX7332 registers)")
endif()

if(DEBUG_ENABLED)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/logging.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE DEBUG_ENABLED)
    message(STATUS "Debug logging: ENABLED")
else()
    message(STATUS "Debug logging: DISABLED")
endif()

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_BINARY_DIR}/generated
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
    $<$<NOT:$<BOOL:${BARE_METAL}>>:USER_VECT_TAB_ADDRESS>
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# Linker script (conditional on BOOTLOADER_BUILD)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "-T${LINKER_SCRIPT}")
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES LINK_DEPENDS "${LINKER_SCRIPT}")

# Generate hex and bin files after build
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMENT "Generating .hex and .bin files"
)
