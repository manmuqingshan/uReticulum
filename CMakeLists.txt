cmake_minimum_required(VERSION 3.16)
project(RTReticulum LANGUAGES C CXX)

set(RTRETICULUM_PORT "posix" CACHE STRING "HAL backend: posix | freertos")
set_property(CACHE RTRETICULUM_PORT PROPERTY STRINGS posix freertos)

option(RTRETICULUM_BUILD_EXAMPLES "Build smoke example(s)" ON)

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(MbedTLS QUIET)
if(NOT MbedTLS_FOUND)
    find_library(MBEDTLS_CRYPTO_LIB NAMES mbedcrypto REQUIRED)
    find_library(MBEDTLS_X509_LIB   NAMES mbedx509)
    find_library(MBEDTLS_TLS_LIB    NAMES mbedtls)
    find_path(MBEDTLS_INCLUDE_DIR mbedtls/sha256.h REQUIRED)
endif()

find_library(MONOCYPHER_LIB NAMES monocypher REQUIRED)
find_path(MONOCYPHER_INCLUDE_DIR monocypher.h REQUIRED)

add_library(rtreticulum STATIC
    src/log.cpp
    src/os.cpp
    src/crc.cpp
    src/memory.cpp
    src/bytes.cpp
    src/cryptography/hashes.cpp
    src/cryptography/hmac.cpp
    src/cryptography/hkdf.cpp
    src/cryptography/aes.cpp
    src/cryptography/ed25519.cpp
    src/cryptography/x25519.cpp
    src/cryptography/fernet.cpp
    src/cryptography/token.cpp
    src/identity.cpp
    src/destination.cpp
    src/packet.cpp
    src/interface.cpp
    src/transport.cpp
    src/link.cpp
    src/resource.cpp
    src/msgpack.cpp
    src/reticulum.cpp
    src/filesystem.cpp
    src/filesystems/posix.cpp
    src/interfaces/loopback.cpp
    third_party/tlsf/tlsf.c
)

target_include_directories(rtreticulum
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tlsf
)

if(MbedTLS_FOUND)
    target_link_libraries(rtreticulum PUBLIC MbedTLS::mbedcrypto)
else()
    target_include_directories(rtreticulum PUBLIC ${MBEDTLS_INCLUDE_DIR})
    target_link_libraries(rtreticulum PUBLIC ${MBEDTLS_CRYPTO_LIB})
endif()

target_include_directories(rtreticulum PUBLIC ${MONOCYPHER_INCLUDE_DIR})
target_link_libraries(rtreticulum PUBLIC ${MONOCYPHER_LIB})

target_compile_options(rtreticulum PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

if(RTRETICULUM_PORT STREQUAL "posix")
    add_library(rtreticulum_hal STATIC port/posix/hal_posix.c)
    target_include_directories(rtreticulum_hal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    find_package(Threads REQUIRED)
    target_link_libraries(rtreticulum_hal PUBLIC Threads::Threads)
    target_link_libraries(rtreticulum PUBLIC rtreticulum_hal)

elseif(RTRETICULUM_PORT STREQUAL "freertos")
    if(NOT TARGET freertos_kernel)
        message(WARNING
            "RTRETICULUM_PORT=freertos but target 'freertos_kernel' is not defined. "
            "Link your firmware against a FreeRTOS kernel target providing "
            "FreeRTOS.h, task.h, semphr.h.")
    endif()
    add_library(rtreticulum_hal STATIC port/freertos/hal_freertos.c)
    target_include_directories(rtreticulum_hal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(TARGET freertos_kernel)
        target_link_libraries(rtreticulum_hal PUBLIC freertos_kernel)
    endif()
    target_link_libraries(rtreticulum PUBLIC rtreticulum_hal)

else()
    message(FATAL_ERROR "Unknown RTRETICULUM_PORT='${RTRETICULUM_PORT}' (expected posix|freertos)")
endif()

if(RTRETICULUM_BUILD_EXAMPLES AND RTRETICULUM_PORT STREQUAL "posix")
    add_executable(rtreticulum_smoke examples/smoke/main.cpp)
    target_link_libraries(rtreticulum_smoke PRIVATE rtreticulum)
endif()

option(RTRETICULUM_BUILD_TESTS "Build host unit tests with doctest" ON)

if(RTRETICULUM_BUILD_TESTS AND RTRETICULUM_PORT STREQUAL "posix")
    add_executable(rtreticulum_tests
        test/test_main.cpp
        test/test_crc.cpp
        test/test_bytes.cpp
        test/test_pkcs7.cpp
        test/test_tlsf.cpp
        test/test_crypto.cpp
        test/test_identity.cpp
        test/test_loopback.cpp
        test/test_transport.cpp
        test/test_link.cpp
        test/test_filesystem.cpp
        test/test_reticulum_task.cpp
        test/test_transport_concurrency.cpp
        test/test_resource.cpp
    )
    target_include_directories(rtreticulum_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest
    )
    target_link_libraries(rtreticulum_tests PRIVATE rtreticulum)
    enable_testing()
    add_test(NAME rtreticulum_tests COMMAND rtreticulum_tests)
endif()
