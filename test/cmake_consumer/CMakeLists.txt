cmake_minimum_required(VERSION 3.16)

project(archbird_external_consumer LANGUAGES C)

find_package(Archbird CONFIG REQUIRED)

add_executable(archbird_external_consumer main.c)
target_compile_features(archbird_external_consumer PRIVATE c_std_11)
target_link_libraries(archbird_external_consumer PRIVATE Archbird::archbird)

enable_testing()
add_test(NAME archbird_external_consumer COMMAND archbird_external_consumer)

if(TARGET Archbird::archbird_static)
  add_executable(archbird_external_static_consumer main.c)
  target_compile_features(archbird_external_static_consumer PRIVATE c_std_11)
  target_link_libraries(archbird_external_static_consumer
                        PRIVATE Archbird::archbird_static)
  add_test(NAME archbird_external_static_consumer
           COMMAND archbird_external_static_consumer)
endif()
