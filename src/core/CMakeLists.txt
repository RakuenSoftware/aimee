cmake_minimum_required(VERSION 3.16)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AIMEE_CORE_VERSION LIMIT_COUNT 1)
project(aimee_core_c VERSION ${AIMEE_CORE_VERSION} LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

option(AIMEE_CORE_WITH_OPENSSL "Build the OpenSSL connection backend" ON)
option(AIMEE_CORE_WITH_NATIVE_TLS "Build the platform native TLS client API" ON)
set(AIMEE_CORE_CONFIG_NEEDS_OPENSSL OFF)
if(UNIX AND NOT APPLE AND AIMEE_CORE_WITH_NATIVE_TLS AND NOT AIMEE_CORE_WITH_OPENSSL)
    message(FATAL_ERROR "Native TLS on this platform requires AIMEE_CORE_WITH_OPENSSL")
endif()

execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/provide_ouroboros_ring.py"
            --source "${CMAKE_CURRENT_BINARY_DIR}/deps/ouroboros-ring"
    OUTPUT_VARIABLE OURO_SOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE OURO_SOURCE_RC)
if(NOT OURO_SOURCE_RC EQUAL 0 OR OURO_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "Failed to provide pinned ouroboros-ring source for aimee-core")
endif()

set(OURO_BUILD_RING OFF CACHE BOOL "" FORCE)
set(OURO_BUILD_CODEC ON CACHE BOOL "" FORCE)
set(OURO_BUILD_CONNECTION ON CACHE BOOL "" FORCE)
set(OURO_WITH_OPENSSL ${AIMEE_CORE_WITH_OPENSSL} CACHE BOOL "" FORCE)
set(OURO_WITH_NATIVE_TLS ${AIMEE_CORE_WITH_NATIVE_TLS} CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${OURO_SOURCE_DIR}/c" "${CMAKE_CURRENT_BINARY_DIR}/ouroboros-ring-build")

add_library(aimee-core-codec STATIC codec/ouroboros_codec_compat.c)
target_compile_features(aimee-core-codec PUBLIC c_std_11)
target_include_directories(aimee-core-codec PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/codec/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(aimee-core-codec PUBLIC ouroboros-codec)
install(TARGETS aimee-core-codec EXPORT aimee-core-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY codec/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Data-only contracts shared by independently built modules.
add_library(aimee-core-types INTERFACE)
target_include_directories(aimee-core-types INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/types/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
install(TARGETS aimee-core-types EXPORT aimee-core-targets)
install(DIRECTORY types/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

add_library(aimee-core-connection STATIC connection/ouroboros_connection_compat.c)
set_target_properties(aimee-core-connection PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(aimee-core-connection PUBLIC c_std_11)
target_include_directories(aimee-core-connection PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/connection/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(aimee-core-connection PUBLIC ouroboros-connection)
if(AIMEE_CORE_WITH_NATIVE_TLS)
    target_compile_definitions(aimee-core-connection PRIVATE AIMEE_CORE_HAS_NATIVE_TLS)
endif()
if(UNIX AND NOT APPLE AND AIMEE_CORE_WITH_OPENSSL)
    set(AIMEE_CORE_CONFIG_NEEDS_OPENSSL ON)
    target_compile_definitions(aimee-core-connection PRIVATE AIMEE_CORE_HAS_OPENSSL)
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
