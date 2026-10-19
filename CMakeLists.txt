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


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME basic)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Recursively add include directories below a root directory
function(add_recursive_include_dirs target root)
    if(IS_DIRECTORY ${root})
        target_include_directories(${target} PRIVATE ${root})
        file(GLOB children RELATIVE ${root} ${root}/*)
        foreach(child ${children})
            if(IS_DIRECTORY ${root}/${child})
                add_recursive_include_dirs(${target} ${root}/${child})
            endif()
        endforeach()
    endif()
endfunction()

# User application sources
file(GLOB_RECURSE USER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/application/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bsp/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/*.c
)

# SEGGER RTT sources
set(SEGGER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/SEGGER/RTT/SEGGER_RTT.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/SEGGER/RTT/SEGGER_RTT_printf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/SEGGER/RTT/SEGGER_RTT_ASM_ARMv7M.s
)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/ST/ARM/DSP/Lib
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${USER_SOURCES}
    ${SEGGER_SOURCES}
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/ST/ARM/DSP/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/ST/STM32_USB_Device_Library/Core/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS/Source/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/SEGGER/RTT
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/SEGGER/Config
)
add_recursive_include_dirs(${CMAKE_PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/application)
add_recursive_include_dirs(${CMAKE_PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/bsp)
add_recursive_include_dirs(${CMAKE_PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/modules)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    ARM_MATH_CM4
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    STM32_Drivers
    libCMSISDSP.a
    m
)

# Resolve binutils from the directory of the selected toolchain compiler.
# This avoids relying on starm-objcopy/starm-size being present in PATH.
if(CMAKE_C_COMPILER)
    get_filename_component(_TOOLCHAIN_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    find_program(_STARM_OBJCOPY NAMES starm-objcopy HINTS "${_TOOLCHAIN_BIN_DIR}" NO_DEFAULT_PATH)
    find_program(_STARM_SIZE NAMES starm-size HINTS "${_TOOLCHAIN_BIN_DIR}" NO_DEFAULT_PATH)
    if(_STARM_OBJCOPY)
        set(CMAKE_OBJCOPY "${_STARM_OBJCOPY}")
    endif()
    if(_STARM_SIZE)
        set(CMAKE_SIZE "${_STARM_SIZE}")
    endif()
endif()

# Generate hex/bin files and show firmware size
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
)
