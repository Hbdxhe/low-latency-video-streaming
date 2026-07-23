cmake_minimum_required(VERSION 3.22)
project(sclient LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/modules/rendering/opengl/shaders)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/rendering/opengl/shaders/video_renderer.vert
    ${CMAKE_CURRENT_BINARY_DIR}/modules/rendering/opengl/shaders/video_renderer.vert
    COPYONLY
)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/rendering/opengl/shaders/video_renderer.frag
    ${CMAKE_CURRENT_BINARY_DIR}/modules/rendering/opengl/shaders/video_renderer.frag
    COPYONLY
)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SCLIENT_DEPS REQUIRED libavcodec libavutil)
pkg_check_modules(SCLIENT_GLFW REQUIRED glfw3 gl)

set(SCLIENT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SCLIENT_TEST_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SCLIENT_THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

function(sclient_configure_target target_name)
    target_include_directories(${target_name} PUBLIC ${SCLIENT_INCLUDE_DIR})
endfunction()

function(sclient_configure_rendering_target target_name)
    sclient_configure_target(${target_name})
    target_include_directories(${target_name} PRIVATE ${SCLIENT_DEPS_INCLUDE_DIRS})
    target_include_directories(${target_name} PRIVATE ${SCLIENT_GLFW_INCLUDE_DIRS})
    target_include_directories(${target_name} PRIVATE ${SCLIENT_THIRD_PARTY_DIR})
    target_include_directories(${target_name} PRIVATE ${SCLIENT_THIRD_PARTY_DIR}/glad/include)
    target_include_directories(${target_name} PRIVATE ${SCLIENT_THIRD_PARTY_DIR}/imgui)
    target_include_directories(${target_name} PRIVATE ${SCLIENT_THIRD_PARTY_DIR}/imgui/backends)
    target_link_directories(${target_name} PRIVATE ${SCLIENT_DEPS_LIBRARY_DIRS})
    target_link_directories(${target_name} PRIVATE ${SCLIENT_GLFW_LIBRARY_DIRS})
endfunction()

function(sclient_configure_test_target target_name)
    target_include_directories(${target_name} PRIVATE ${SCLIENT_INCLUDE_DIR})
    target_include_directories(${target_name} PRIVATE ${SCLIENT_TEST_INCLUDE_DIR})
endfunction()

add_library(sclient_common
    src/common/log/Logger.cpp
    src/common/net/SdpSessionDescription.cpp
)
sclient_configure_target(sclient_common)

add_library(sclient_app
    src/app/cli/CliOptions.cpp
)
sclient_configure_target(sclient_app)
target_link_libraries(sclient_app PUBLIC sclient_common)

add_library(sclient_network
    src/modules/network/StreamClient.cpp
    src/modules/network/StreamClientRtp.cpp
    src/modules/network/StreamClientTcp.cpp
    src/modules/network/StreamClientUdp.cpp
)
sclient_configure_target(sclient_network)
target_link_libraries(sclient_network PUBLIC sclient_common Threads::Threads)

add_library(sclient_decoding
    src/modules/decoding/VideoDecoder.cpp
    src/modules/decoding/software/SoftwareVideoDecoderBackend.cpp
)
sclient_configure_target(sclient_decoding)
target_include_directories(sclient_decoding PRIVATE ${SCLIENT_DEPS_INCLUDE_DIRS})
target_link_directories(sclient_decoding PRIVATE ${SCLIENT_DEPS_LIBRARY_DIRS})
target_link_libraries(sclient_decoding PUBLIC sclient_common Threads::Threads ${SCLIENT_DEPS_LIBRARIES})

add_library(sclient_rendering
    src/modules/rendering/VideoRenderer.cpp
    src/modules/rendering/opengl/ImGuiOpenGL3Backend.cpp
    src/modules/rendering/opengl/OpenGlVideoRendererBackend.cpp
    src/modules/rendering/opengl/OpenGlVideoRendererHud.cpp
    third_party/glad/src/glad.c
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
)
sclient_configure_rendering_target(sclient_rendering)
target_link_libraries(sclient_rendering PUBLIC sclient_common ${SCLIENT_DEPS_LIBRARIES} ${SCLIENT_GLFW_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS})

add_executable(sclient
    src/app/main.cpp
)
sclient_configure_target(sclient)
target_link_libraries(
    sclient
    PRIVATE
        sclient_app
        sclient_network
        sclient_decoding
        sclient_rendering
        Threads::Threads
)

if(BUILD_TESTING)
    add_executable(cli_options_test
        tests/unit/CliOptionsTest.cpp
    )

    sclient_configure_test_target(cli_options_test)
    target_link_libraries(cli_options_test PRIVATE sclient_app)

    add_executable(udp_receive_integration_test
        tests/integration/UdpReceiveIntegrationTest.cpp
    )

    sclient_configure_test_target(udp_receive_integration_test)
    target_link_libraries(udp_receive_integration_test PRIVATE sclient_network)

    add_executable(tcp_receive_integration_test
        tests/integration/TcpReceiveIntegrationTest.cpp
    )

    sclient_configure_test_target(tcp_receive_integration_test)
    target_link_libraries(tcp_receive_integration_test PRIVATE sclient_network)

    add_executable(udp_jitter_benchmark
        tests/benchmark/UdpJitterBenchmark.cpp
    )

    sclient_configure_test_target(udp_jitter_benchmark)
    target_link_libraries(udp_jitter_benchmark PRIVATE sclient_app sclient_network sclient_decoding)

    add_executable(rtp_decode_smoke
        tests/smoke/RtpDecodeSmoke.cpp
    )

    sclient_configure_test_target(rtp_decode_smoke)
    target_link_libraries(rtp_decode_smoke PRIVATE sclient_app sclient_network sclient_decoding)

    add_executable(rtp_receive_integration_test
        tests/integration/RtpReceiveIntegrationTest.cpp
    )

    sclient_configure_test_target(rtp_receive_integration_test)
    target_link_libraries(rtp_receive_integration_test PRIVATE sclient_network)

    add_executable(sdp_session_description_test
        tests/unit/SdpSessionDescriptionTest.cpp
    )

    sclient_configure_test_target(sdp_session_description_test)
    target_link_libraries(sdp_session_description_test PRIVATE sclient_common)

    add_executable(rtp_protocol_test
        tests/unit/RtpProtocolTest.cpp
    )

    sclient_configure_test_target(rtp_protocol_test)

    add_executable(bounded_queue_test
        tests/unit/BoundedQueueTest.cpp
    )

    sclient_configure_test_target(bounded_queue_test)
    target_link_libraries(bounded_queue_test PRIVATE Threads::Threads)

    add_executable(latency_stats_test
        tests/unit/LatencyStatsTest.cpp
    )

    sclient_configure_test_target(latency_stats_test)

    add_test(NAME cli_options_test COMMAND cli_options_test)
    set_tests_properties(cli_options_test PROPERTIES LABELS "unit;cli" TIMEOUT 5)

    add_test(NAME sclient_help COMMAND sclient --help)
    set_tests_properties(sclient_help PROPERTIES LABELS "cli" TIMEOUT 5)

    add_test(NAME tcp_receive_integration_test COMMAND tcp_receive_integration_test)
    set_tests_properties(tcp_receive_integration_test PROPERTIES LABELS "integration;tcp" TIMEOUT 10)

    add_test(NAME udp_receive_integration_test COMMAND udp_receive_integration_test)
    set_tests_properties(udp_receive_integration_test PROPERTIES LABELS "integration;udp" TIMEOUT 15)

    add_test(NAME rtp_receive_integration_test COMMAND rtp_receive_integration_test)
    set_tests_properties(rtp_receive_integration_test PROPERTIES LABELS "integration;rtp" TIMEOUT 10)

    add_test(NAME sdp_session_description_test COMMAND sdp_session_description_test)
    set_tests_properties(sdp_session_description_test PROPERTIES LABELS "unit;net" TIMEOUT 5)

    add_test(NAME rtp_protocol_test COMMAND rtp_protocol_test)
    set_tests_properties(rtp_protocol_test PROPERTIES LABELS "unit;rtp" TIMEOUT 5)

    add_test(NAME bounded_queue_test COMMAND bounded_queue_test)
    set_tests_properties(bounded_queue_test PROPERTIES LABELS "unit" TIMEOUT 5)

    add_test(NAME latency_stats_test COMMAND latency_stats_test)
    set_tests_properties(latency_stats_test PROPERTIES LABELS "unit" TIMEOUT 5)

    add_test(NAME rtp_decode_smoke COMMAND rtp_decode_smoke)
    set_tests_properties(rtp_decode_smoke PROPERTIES LABELS "smoke;rtp" TIMEOUT 10)

    add_test(NAME udp_jitter_benchmark_help COMMAND udp_jitter_benchmark --help)
    set_tests_properties(udp_jitter_benchmark_help PROPERTIES LABELS "benchmark;cli" TIMEOUT 5)

    add_test(NAME rtp_decode_smoke_help COMMAND rtp_decode_smoke --help)
    set_tests_properties(rtp_decode_smoke_help PROPERTIES LABELS "cli" TIMEOUT 5)
endif()
