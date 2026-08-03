cmake_minimum_required(VERSION 3.16)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AIMEE_CORE_VERSION LIMIT_COUNT 1)
project(aimee_core_c VERSION ${AIMEE_CORE_VERSION} LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(AIMEE_CORE_WITH_OPENSSL "Build the OpenSSL connection backend" ON)
option(AIMEE_CORE_WITH_NATIVE_TLS "Build the platform native TLS client API" ON)
set(AIMEE_CORE_CONFIG_NEEDS_OPENSSL OFF)
set(AIMEE_CORE_EVENT_BUS_ENABLED OFF)
if(UNIX AND NOT APPLE AND AIMEE_CORE_WITH_NATIVE_TLS AND NOT AIMEE_CORE_WITH_OPENSSL)
    message(FATAL_ERROR "Native TLS on this platform requires AIMEE_CORE_WITH_OPENSSL")
endif()

add_library(aimee-core-connection STATIC
    connection/auth.c
    connection/control.c
    connection/endpoint.c
    connection/http1.c
    connection/socket.c
)
target_compile_features(aimee-core-connection PUBLIC c_std_11)
target_include_directories(aimee-core-connection PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/connection/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(AIMEE_CORE_WITH_NATIVE_TLS)
    target_sources(aimee-core-connection PRIVATE
        connection/native_tls_identity.c
        connection/native_tls_path.c)
endif()
if(WIN32)
    target_compile_definitions(aimee-core-connection PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(aimee-core-connection PUBLIC ws2_32)
    if(AIMEE_CORE_WITH_NATIVE_TLS)
        target_sources(aimee-core-connection PRIVATE connection/native_tls_schannel.c)
        target_link_libraries(aimee-core-connection PUBLIC secur32 crypt32 ncrypt)
    endif()
elseif(APPLE)
    if(AIMEE_CORE_WITH_NATIVE_TLS)
        target_sources(aimee-core-connection PRIVATE connection/native_tls_securetransport.c)
        set_source_files_properties(connection/native_tls_securetransport.c
            PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations")
        find_library(SECURITY_FRAMEWORK Security REQUIRED)
        find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
        target_link_libraries(aimee-core-connection PUBLIC
            ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
    endif()
elseif(AIMEE_CORE_WITH_OPENSSL)
    find_package(OpenSSL REQUIRED)
    set(AIMEE_CORE_CONFIG_NEEDS_OPENSSL ON)
    target_sources(aimee-core-connection PRIVATE connection/tls_openssl.c)
    if(AIMEE_CORE_WITH_NATIVE_TLS)
        target_sources(aimee-core-connection PRIVATE connection/native_tls_openssl.c)
    endif()
    target_link_libraries(aimee-core-connection PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(AIMEE_CORE_EVENT_BUS_ENABLED ON)
    find_package(Threads REQUIRED)
    add_library(aimee-core-event-bus-client STATIC
        event_bus/bus_attach.c
        event_bus/bus_client.c
        event_bus/bus_endpoint.c
        event_bus/module_client.c
        event_bus/module_protocol.c
        event_bus/module_runtime.c
        event_bus/bus_region.c
        event_bus/bus_ring.c
        event_bus/bus_wire.c
    )
    target_compile_features(aimee-core-event-bus-client PUBLIC c_std_11)
    target_include_directories(aimee-core-event-bus-client PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/event_bus/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(aimee-core-event-bus-client PUBLIC Threads::Threads)

    add_library(aimee-core-event-bus STATIC
        event_bus/bus_attach.c
        event_bus/bus_arena.c
        event_bus/bus_capture.c
        event_bus/bus_client.c
        event_bus/bus_endpoint.c
        event_bus/bus_host.c
        event_bus/module_client.c
        event_bus/module_protocol.c
        event_bus/module_runtime.c
        event_bus/bus_region.c
        event_bus/bus_region_host.c
        event_bus/bus_ring.c
        event_bus/bus_route.c
        event_bus/bus_runtime.c
        event_bus/bus_wire.c
    )
    target_compile_features(aimee-core-event-bus PUBLIC c_std_11)
    target_include_directories(aimee-core-event-bus PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/event_bus/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(aimee-core-event-bus PUBLIC Threads::Threads)
    install(TARGETS aimee-core-event-bus aimee-core-event-bus-client EXPORT aimee-core-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY event_bus/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

install(TARGETS aimee-core-connection EXPORT aimee-core-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY connection/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT aimee-core-targets NAMESPACE aimee::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aimee-core)
configure_package_config_file(
    cmake/aimee-core-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/aimee-core-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aimee-core)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/aimee-core-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    VERSION
    ${CMAKE_CURRENT_BINARY_DIR}/aimee-core-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/aimee-core-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aimee-core)
