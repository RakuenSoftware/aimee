cmake_minimum_required(VERSION 3.16)
project(aimee_connection_consumer LANGUAGES C)

find_package(aimee-core 0.2.0 EXACT CONFIG REQUIRED)
add_executable(aimee-connection-consumer main.c)
target_compile_features(aimee-connection-consumer PRIVATE c_std_11)
target_link_libraries(aimee-connection-consumer PRIVATE aimee::aimee-core-connection)
