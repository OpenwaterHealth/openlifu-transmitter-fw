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

# version.h is configured below, once FDA_MODE sets the build variant.

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
#                              LENGTH=155K (FDA: 151K), VTOR=0x08010400 (slot + signed-header
#                              offset; uses STM32L443XX_FLASH.ld). This is the
#                              image that gets signed.
#   BARE_METAL=ON            : standalone image at 0x08000000, LENGTH=220K (FDA: 216K), no
#                              bootloader (uses STM32L443RCIX_FLASH.ld).
option(BARE_METAL "Build a bare-metal image (no bootloader; runs at 0x08000000)" OFF)

if(BARE_METAL)
    message(STATUS "Bare-metal build : FLASH origin=0x08000000, LENGTH=220K (FDA: 216K)")
    set(LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/STM32L443RCIX_FLASH.ld")
else()
    message(STATUS "Bootloader build : FLASH origin=0x08010400, LENGTH=155K (FDA: 151K), VTOR=0x08010400 (SBSFU slot + signed-header offset)")
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
    Core/Src/preset_policy.c
)

# ---------------------------
# FDA mode
# ---------------------------
# Two builds (Core/Inc/presets.h):
#
# FDA_MODE links one read-only preset into the end of the bootloader's slot,
# pages 108-109, and is always preset-locked: the host loads the preset and
# can change nothing else. Its header, Core/Inc/presets/fda_preset.h, comes
# from the openlifu SDK's examples/generate_presets.py.
#
# Every other build is DEV: full host control, presets in pages 110-125, and
# a runtime lock (OW_PRESET_LOCK) that refuses what FDA_MODE refuses until the
# next reset. Only a bare-metal DEV image can store presets, since the
# bootloader's slot still covers pages 110-125.
option(FDA_MODE "One read-only preset, linked in at the end of the application slot" OFF)

# A presets-only (FDA_MODE) image reports which preset set it carries:
# "fda" (the released preset) or "test" (the test presets). It must match a
# preset context in openlifu-diathermy-application.
set(FW_PRESET_VARIANT "fda" CACHE STRING "Preset set in a presets-only image: fda or test")
set_property(CACHE FW_PRESET_VARIANT PROPERTY STRINGS fda test)
if(NOT FW_PRESET_VARIANT MATCHES "^(fda|test)$")
    message(FATAL_ERROR "FW_PRESET_VARIANT must be fda or test, not '${FW_PRESET_VARIANT}'.")
endif()

if(FDA_MODE)
    set(FDA_PRESET_HEADER "${CMAKE_SOURCE_DIR}/Core/Inc/presets/fda_preset.h")
    if(NOT EXISTS "${FDA_PRESET_HEADER}")
        message(FATAL_ERROR "FDA_MODE needs ${FDA_PRESET_HEADER}: generate it with the openlifu SDK's "
                            "examples/generate_presets.py.")
    endif()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE FDA_MODE)
    set(FW_BUILD_VARIANT "${FW_PRESET_VARIANT}")

    message(STATUS "FDA mode    : ON (preset at pages 108-109, always locked)")
else()
    set(FW_BUILD_VARIANT "")
    message(STATUS "FDA mode    : OFF (DEV: host control, preset slots, runtime lock)")
endif()

# OW_CMD_VERSION returns FW_VERSION. A presets-only image appends
# "+<FW_PRESET_VARIANT>" (semver build metadata), so the host can tell which
# preset set it carries without writing to the device. Any other build has no
# suffix: all commands are enabled and the host treats it as a development or
# service image. Whether such an image is locked is runtime state:
# OW_PRESET_LOCK reports it.
if(FW_BUILD_VARIANT)
    set(FW_VERSION_STRING "${FW_GIT_DESCRIBE}+${FW_BUILD_VARIANT}")
else()
    set(FW_VERSION_STRING "${FW_GIT_DESCRIBE}")
endif()

message(STATUS "FW version   : ${FW_VERSION_STRING}")
message(STATUS "FW git sha   : ${FW_GIT_SHA}")
message(STATUS "FW build time: ${FW_BUILD_TIME_UTC}")

configure_file(
    ${CMAKE_SOURCE_DIR}/version.h.in
    ${CMAKE_BINARY_DIR}/generated/version.h
    @ONLY
)

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
# The linker script sizes the app slot and the preset region from FDA_MODE.
if(FDA_MODE)
    set(FDA_LAYOUT 1)
else()
    set(FDA_LAYOUT 0)
endif()
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "-T${LINKER_SCRIPT}" "-Wl,--defsym=FDA_MODE=${FDA_LAYOUT}")
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES LINK_DEPENDS "${LINKER_SCRIPT}")

# Generate hex and bin files after build
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMENT "Generating .hex and .bin files"
)
