cmake_minimum_required(VERSION 3.22)
project(sserver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optimization flags
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

# Architecture detection for SIMD support
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    add_compile_definitions(SSERVER_HAS_NEON=1)
    message(STATUS "ARM64 detected -- NEON enabled")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    add_compile_definitions(SSERVER_HAS_SSE=1)
    message(STATUS "x86_64 detected -- SSE baseline enabled")
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Enable GNU extensions for sendmmsg and other Linux-specific APIs
add_compile_definitions(_GNU_SOURCE)

enable_testing()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(FFMPEG_BENCH REQUIRED libavcodec libavutil libswscale)
pkg_check_modules(OPENCV QUIET opencv4)

set(SSERVER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(sserver_common
    src/common/log/Logger.cpp
    src/common/metrics/LatencyRecorder.cpp
)
target_include_directories(sserver_common PUBLIC ${SSERVER_INCLUDE_DIR})
target_link_libraries(sserver_common PUBLIC Threads::Threads)

add_library(sserver_config
    src/config/AppConfig.cpp
)
target_include_directories(sserver_config PUBLIC ${SSERVER_INCLUDE_DIR})
target_link_libraries(sserver_config PUBLIC sserver_common)

add_library(sserver_core
    src/core/Application.cpp
)
target_include_directories(sserver_core PUBLIC ${SSERVER_INCLUDE_DIR})
target_link_libraries(sserver_core PUBLIC sserver_common sserver_config)

add_library(sserver_encoding
    src/modules/encoding/video/VideoEncoder.cpp
    src/modules/encoding/video/x264/X264VideoEncoderBackend.cpp
)
target_include_directories(sserver_encoding PUBLIC ${SSERVER_INCLUDE_DIR})
target_link_libraries(sserver_encoding PUBLIC sserver_common sserver_config x264 Threads::Threads)

add_library(sserver_modules
    src/modules/capture/video/Capture.cpp
    src/modules/capture/video/CaptureModule.cpp
    src/modules/capture/video/null/NullCaptureDevice.cpp
    src/modules/transport/Transport.cpp
    src/modules/transport/TransportModule.cpp
    src/modules/transport/rtp/RtpPacketizer.cpp
    src/modules/transport/rtp/RtpStreamingBackend.cpp
    src/modules/transport/tcp/TcpClientSession.cpp
    src/modules/transport/tcp/TcpStreamingBackend.cpp
    src/modules/transport/udp/UdpStreamingBackend.cpp
    src/modules/capture/video/v4l2/V4L2CaptureDevice.cpp
)
target_include_directories(sserver_modules PUBLIC ${SSERVER_INCLUDE_DIR})
target_link_libraries(sserver_modules PUBLIC sserver_common sserver_config sserver_core sserver_encoding Threads::Threads)

add_library(sserver_app
    src/app/AppBootstrap.cpp
)
target_include_directories(sserver_app PUBLIC ${SSERVER_INCLUDE_DIR})
target_link_libraries(sserver_app
    PUBLIC
        sserver_common
        sserver_config
        sserver_core
        sserver_modules
)

add_executable(stream_server
    src/app/main.cpp
)
target_include_directories(stream_server PRIVATE ${SSERVER_INCLUDE_DIR})
target_link_libraries(stream_server PRIVATE sserver_app)

add_executable(smoke_test
    tests/smoke/SmokeTest.cpp
)
target_include_directories(smoke_test PRIVATE ${SSERVER_INCLUDE_DIR})
target_compile_definitions(smoke_test PRIVATE SSERVER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(smoke_test PRIVATE sserver_app)

add_executable(transport_integration_test
    tests/integration/TransportIntegrationTest.cpp
)
target_include_directories(transport_integration_test PRIVATE ${SSERVER_INCLUDE_DIR})
target_include_directories(transport_integration_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(transport_integration_test PRIVATE SSERVER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(transport_integration_test PRIVATE sserver_app)

add_executable(rtp_integration_test
    tests/integration/RtpIntegrationTest.cpp
)
target_include_directories(rtp_integration_test PRIVATE ${SSERVER_INCLUDE_DIR})
target_include_directories(rtp_integration_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(rtp_integration_test PRIVATE SSERVER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rtp_integration_test PRIVATE sserver_app)

add_executable(rtp_protocol_test
    tests/unit/RtpProtocolTest.cpp
)
target_include_directories(rtp_protocol_test PRIVATE ${SSERVER_INCLUDE_DIR})

add_executable(rtp_config_test
    tests/unit/RtpConfigTest.cpp
)
target_include_directories(rtp_config_test PRIVATE ${SSERVER_INCLUDE_DIR})
target_link_libraries(rtp_config_test PRIVATE sserver_config)

add_executable(udp_config_test
    tests/unit/UdpConfigTest.cpp
)
target_include_directories(udp_config_test PRIVATE ${SSERVER_INCLUDE_DIR})
target_link_libraries(udp_config_test PRIVATE sserver_config)

add_executable(udp_profile_config_test
    tests/unit/UdpProfileConfigTest.cpp
)
target_include_directories(udp_profile_config_test PRIVATE ${SSERVER_INCLUDE_DIR})
target_compile_definitions(udp_profile_config_test PRIVATE SSERVER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(udp_profile_config_test PRIVATE sserver_config)

add_executable(config_loader_negative_test
    tests/unit/ConfigLoaderNegativeTest.cpp
)
target_include_directories(config_loader_negative_test PRIVATE ${SSERVER_INCLUDE_DIR})
target_link_libraries(config_loader_negative_test PRIVATE sserver_config)

add_executable(udp_fec_recovery_test
    tests/unit/UdpFecRecoveryTest.cpp
)
target_include_directories(udp_fec_recovery_test PRIVATE ${SSERVER_INCLUDE_DIR})
target_include_directories(udp_fec_recovery_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(udp_fec_recovery_test PRIVATE sserver_app)

add_executable(application_lifecycle_test
    tests/unit/ApplicationLifecycleTest.cpp
)
target_include_directories(application_lifecycle_test PRIVATE ${SSERVER_INCLUDE_DIR})
target_link_libraries(application_lifecycle_test PRIVATE sserver_core sserver_config)

add_executable(latency_recorder_test
    tests/unit/LatencyRecorderTest.cpp
)
target_include_directories(latency_recorder_test PRIVATE ${SSERVER_INCLUDE_DIR})
target_link_libraries(latency_recorder_test PRIVATE sserver_common)

add_executable(transport_benchmark
    tests/benchmark/TransportBenchmark.cpp
)
target_include_directories(transport_benchmark PRIVATE ${SSERVER_INCLUDE_DIR})
target_include_directories(transport_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(transport_benchmark PRIVATE SSERVER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(transport_benchmark PRIVATE sserver_app)

add_executable(latency_benchmark
    tests/latency/DecodeDisplayProbe.cpp
    tests/latency/LatencyBenchmark.cpp
)
target_include_directories(latency_benchmark PRIVATE ${SSERVER_INCLUDE_DIR})
target_include_directories(latency_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(latency_benchmark PRIVATE SSERVER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(latency_benchmark PRIVATE ${FFMPEG_BENCH_INCLUDE_DIRS})
target_link_directories(latency_benchmark PRIVATE ${FFMPEG_BENCH_LIBRARY_DIRS})
target_link_libraries(latency_benchmark PRIVATE sserver_app ${FFMPEG_BENCH_LIBRARIES})
if(OPENCV_FOUND)
    target_include_directories(latency_benchmark PRIVATE ${OPENCV_INCLUDE_DIRS})
    target_link_directories(latency_benchmark PRIVATE ${OPENCV_LIBRARY_DIRS})
    target_link_libraries(latency_benchmark PRIVATE ${OPENCV_LIBRARIES})
    target_compile_definitions(latency_benchmark PRIVATE SSERVER_HAS_OPENCV)
endif()

add_test(NAME smoke_test COMMAND smoke_test)
set_tests_properties(smoke_test PROPERTIES LABELS "smoke" TIMEOUT 10)

add_test(
    NAME smoke_udp_test
    COMMAND smoke_test --config ${CMAKE_CURRENT_SOURCE_DIR}/config/smoke_udp.conf
)
set_tests_properties(smoke_udp_test PROPERTIES LABELS "smoke;udp" TIMEOUT 10)

add_test(
    NAME integration_transport_tcp
    COMMAND transport_integration_test --config ${CMAKE_CURRENT_SOURCE_DIR}/config/integration_tcp.conf
)
set_tests_properties(integration_transport_tcp PROPERTIES LABELS "integration" TIMEOUT 15)

add_test(
    NAME integration_transport_udp
    COMMAND transport_integration_test --config ${CMAKE_CURRENT_SOURCE_DIR}/config/integration_udp.conf
)
set_tests_properties(integration_transport_udp PROPERTIES LABELS "integration" TIMEOUT 15)

add_test(
    NAME integration_transport_udp_fec
    COMMAND transport_integration_test --config ${CMAKE_CURRENT_SOURCE_DIR}/config/integration_udp_fec.conf
)
set_tests_properties(integration_transport_udp_fec PROPERTIES LABELS "integration" TIMEOUT 15)

add_test(
    NAME integration_transport_udp_fec_nack
    COMMAND transport_integration_test --config ${CMAKE_CURRENT_SOURCE_DIR}/config/integration_udp_fec_nack.conf
)
set_tests_properties(integration_transport_udp_fec_nack PROPERTIES LABELS "integration;udp" TIMEOUT 15)

add_test(
    NAME integration_rtp_null
    COMMAND rtp_integration_test --config ${CMAKE_CURRENT_SOURCE_DIR}/config/integration_rtp_null.conf
)
set_tests_properties(integration_rtp_null PROPERTIES LABELS "integration;rtp" TIMEOUT 20)

add_test(
    NAME integration_rtp_v4l2
    COMMAND rtp_integration_test --config ${CMAKE_CURRENT_SOURCE_DIR}/config/integration_rtp_v4l2.conf
)
set_tests_properties(
    integration_rtp_v4l2
    PROPERTIES
        LABELS "integration;rtp;hardware"
        TIMEOUT 25
        SKIP_RETURN_CODE 77
)

add_test(NAME rtp_protocol_test COMMAND rtp_protocol_test)
set_tests_properties(rtp_protocol_test PROPERTIES LABELS "unit;rtp" TIMEOUT 10)

add_test(NAME rtp_config_test COMMAND rtp_config_test)
set_tests_properties(rtp_config_test PROPERTIES LABELS "unit;rtp" TIMEOUT 10)

add_test(NAME udp_config_test COMMAND udp_config_test)
set_tests_properties(udp_config_test PROPERTIES LABELS "unit;udp" TIMEOUT 10)

add_test(NAME udp_profile_config_test COMMAND udp_profile_config_test)
set_tests_properties(udp_profile_config_test PROPERTIES LABELS "unit;udp" TIMEOUT 10)

add_test(NAME config_loader_negative_test COMMAND config_loader_negative_test)
set_tests_properties(config_loader_negative_test PROPERTIES LABELS "unit;config" TIMEOUT 10)

add_test(NAME udp_fec_recovery_test COMMAND udp_fec_recovery_test)
set_tests_properties(udp_fec_recovery_test PROPERTIES LABELS "unit;udp;fec" TIMEOUT 10)

add_test(NAME application_lifecycle_test COMMAND application_lifecycle_test)
set_tests_properties(application_lifecycle_test PROPERTIES LABELS "unit;core" TIMEOUT 10)

add_test(NAME latency_recorder_test COMMAND latency_recorder_test)
set_tests_properties(latency_recorder_test PROPERTIES LABELS "unit;metrics" TIMEOUT 10)

add_test(
    NAME benchmark_transport_tcp
    COMMAND transport_benchmark --config ${CMAKE_CURRENT_SOURCE_DIR}/config/benchmark_null_tcp.conf
)
set_tests_properties(benchmark_transport_tcp PROPERTIES LABELS "benchmark" TIMEOUT 20)

add_test(
    NAME benchmark_transport_tcp_drop_oldest
    COMMAND transport_benchmark
            --config ${CMAKE_CURRENT_SOURCE_DIR}/config/benchmark_null_tcp_drop_oldest.conf
            --receive-delay-ms 20
)
set_tests_properties(benchmark_transport_tcp_drop_oldest PROPERTIES LABELS "benchmark" TIMEOUT 30)

add_test(
    NAME benchmark_transport_tcp_drop_oldest_non_key
    COMMAND transport_benchmark
            --config ${CMAKE_CURRENT_SOURCE_DIR}/config/benchmark_null_tcp_drop_oldest_non_key.conf
            --receive-delay-ms 20
)
set_tests_properties(benchmark_transport_tcp_drop_oldest_non_key PROPERTIES LABELS "benchmark" TIMEOUT 30)

add_test(
    NAME benchmark_transport_udp
    COMMAND transport_benchmark --config ${CMAKE_CURRENT_SOURCE_DIR}/config/benchmark_null_udp.conf
)
set_tests_properties(benchmark_transport_udp PROPERTIES LABELS "benchmark" TIMEOUT 20)

add_test(
    NAME benchmark_transport_rtp
    COMMAND transport_benchmark --config ${CMAKE_CURRENT_SOURCE_DIR}/config/benchmark_null_rtp.conf
)
set_tests_properties(benchmark_transport_rtp PROPERTIES LABELS "benchmark;rtp" TIMEOUT 20)

add_test(
    NAME latency_benchmark_rtp_null
    COMMAND latency_benchmark --config ${CMAKE_CURRENT_SOURCE_DIR}/config/benchmark_rtp_null.conf
)
set_tests_properties(latency_benchmark_rtp_null PROPERTIES LABELS "latency;rtp" TIMEOUT 25)

add_test(
    NAME latency_benchmark_tcp
    COMMAND latency_benchmark --config ${CMAKE_CURRENT_SOURCE_DIR}/config/benchmark.conf
)
set_tests_properties(
    latency_benchmark_tcp
    PROPERTIES
        LABELS "latency;tcp;hardware"
        TIMEOUT 30
        SKIP_RETURN_CODE 77
)

add_test(
    NAME latency_benchmark_udp
    COMMAND latency_benchmark --config ${CMAKE_CURRENT_SOURCE_DIR}/config/benchmark_udp.conf
)
set_tests_properties(
    latency_benchmark_udp
    PROPERTIES
        LABELS "latency;udp;hardware"
        TIMEOUT 30
        SKIP_RETURN_CODE 77
)

add_test(
    NAME latency_benchmark_rtp_v4l2
    COMMAND latency_benchmark --config ${CMAKE_CURRENT_SOURCE_DIR}/config/benchmark_rtp_v4l2.conf
)
set_tests_properties(
    latency_benchmark_rtp_v4l2
    PROPERTIES
        LABELS "latency;rtp;hardware"
        TIMEOUT 30
        SKIP_RETURN_CODE 77
)

add_test(
    NAME latency_benchmark_invalid_udp_no_metadata
    COMMAND latency_benchmark --config ${CMAKE_CURRENT_SOURCE_DIR}/config/benchmark_invalid_udp_no_metadata.conf
)
set_tests_properties(
    latency_benchmark_invalid_udp_no_metadata
    PROPERTIES
        LABELS "latency;negative;udp"
        TIMEOUT 10
        WILL_FAIL TRUE
)

add_test(
    NAME latency_benchmark_invalid_rtp_no_extension
    COMMAND latency_benchmark --config ${CMAKE_CURRENT_SOURCE_DIR}/config/benchmark_invalid_rtp_no_extension.conf
)
set_tests_properties(
    latency_benchmark_invalid_rtp_no_extension
    PROPERTIES
        LABELS "latency;negative;rtp"
        TIMEOUT 10
        WILL_FAIL TRUE
)
