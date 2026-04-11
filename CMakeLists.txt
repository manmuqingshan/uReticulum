cmake_minimum_required(VERSION 3.16)
project(uReticulum LANGUAGES C CXX)

set(URETICULUM_PORT "posix" CACHE STRING "HAL backend: posix | freertos")
set_property(CACHE URETICULUM_PORT PROPERTY STRINGS posix freertos)

option(URETICULUM_BUILD_EXAMPLES "Build smoke example(s)" ON)

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(ureticulum STATIC
    src/log.cpp
    src/os.cpp
)

target_include_directories(ureticulum
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(ureticulum PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

if(URETICULUM_PORT STREQUAL "posix")
    add_library(ureticulum_hal STATIC port/posix/hal_posix.c)
    target_include_directories(ureticulum_hal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    find_package(Threads REQUIRED)
    target_link_libraries(ureticulum_hal PUBLIC Threads::Threads)
    target_link_libraries(ureticulum PUBLIC ureticulum_hal)

elseif(URETICULUM_PORT STREQUAL "freertos")
    if(NOT TARGET freertos_kernel)
        message(WARNING
            "URETICULUM_PORT=freertos but target 'freertos_kernel' is not defined. "
            "Link your firmware against a FreeRTOS kernel target providing "
            "FreeRTOS.h, task.h, semphr.h.")
    endif()
    add_library(ureticulum_hal STATIC port/freertos/hal_freertos.c)
    target_include_directories(ureticulum_hal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(TARGET freertos_kernel)
        target_link_libraries(ureticulum_hal PUBLIC freertos_kernel)
    endif()
    target_link_libraries(ureticulum PUBLIC ureticulum_hal)

else()
    message(FATAL_ERROR "Unknown URETICULUM_PORT='${URETICULUM_PORT}' (expected posix|freertos)")
endif()

if(URETICULUM_BUILD_EXAMPLES AND URETICULUM_PORT STREQUAL "posix")
    add_executable(ureticulum_smoke examples/smoke/main.cpp)
    target_link_libraries(ureticulum_smoke PRIVATE ureticulum)
endif()
