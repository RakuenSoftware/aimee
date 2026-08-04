cmake_minimum_required(VERSION 3.16)
project(aimee_event_bus_module LANGUAGES C)

find_package(aimee-core 0.3.0 EXACT CONFIG REQUIRED)
add_executable(aimee-event-bus-module main.c)
target_compile_features(aimee-event-bus-module PRIVATE c_std_11)
target_link_libraries(aimee-event-bus-module PRIVATE aimee::aimee-core-event-bus-client)
