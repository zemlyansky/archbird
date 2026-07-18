if(NOT DEFINED ARCHBIRD_BUILD_DIR OR NOT DEFINED ARCHBIRD_SOURCE_DIR OR
   NOT DEFINED ARCHBIRD_TEST_ROOT OR NOT DEFINED ARCHBIRD_INSTALL_LIBDIR OR
   NOT DEFINED ARCHBIRD_C_COMPILER)
  message(FATAL_ERROR "install-consumer test inputs are incomplete")
endif()

set(stage "${ARCHBIRD_TEST_ROOT}/prefix")
set(consumer_build "${ARCHBIRD_TEST_ROOT}/consumer")
file(REMOVE_RECURSE "${ARCHBIRD_TEST_ROOT}")
file(MAKE_DIRECTORY "${ARCHBIRD_TEST_ROOT}")

set(config_args)
set(ctest_config_args)
if(DEFINED ARCHBIRD_BUILD_CONFIG AND NOT ARCHBIRD_BUILD_CONFIG STREQUAL "")
  list(APPEND config_args --config "${ARCHBIRD_BUILD_CONFIG}")
  list(APPEND ctest_config_args -C "${ARCHBIRD_BUILD_CONFIG}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${ARCHBIRD_BUILD_DIR}"
          --prefix "${stage}" ${config_args}
  RESULT_VARIABLE install_status
  OUTPUT_VARIABLE install_stdout
  ERROR_VARIABLE install_stderr)
if(NOT install_status EQUAL 0)
  message(FATAL_ERROR
          "Archbird staged install failed:\n${install_stdout}\n${install_stderr}")
endif()

foreach(required IN ITEMS
        "${stage}/include/archbird/archbird.h"
        "${stage}/${ARCHBIRD_INSTALL_LIBDIR}/pkgconfig/archbird.pc"
        "${stage}/${ARCHBIRD_INSTALL_LIBDIR}/cmake/Archbird/ArchbirdConfig.cmake"
        "${stage}/${ARCHBIRD_INSTALL_LIBDIR}/cmake/Archbird/ArchbirdTargets.cmake")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "staged install omitted ${required}")
  endif()
endforeach()

find_program(pkg_config_executable pkg-config)
if(pkg_config_executable)
  set(pkg_config_path "${stage}/${ARCHBIRD_INSTALL_LIBDIR}/pkgconfig")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${pkg_config_path}"
            "${pkg_config_executable}" --cflags --libs archbird
    RESULT_VARIABLE pkg_config_status
    OUTPUT_VARIABLE pkg_config_flags
    ERROR_VARIABLE pkg_config_stderr
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT pkg_config_status EQUAL 0)
    message(FATAL_ERROR "installed pkg-config lookup failed: ${pkg_config_stderr}")
  endif()
  separate_arguments(pkg_config_arguments NATIVE_COMMAND "${pkg_config_flags}")
  execute_process(
    COMMAND "${ARCHBIRD_C_COMPILER}"
            "${ARCHBIRD_SOURCE_DIR}/test/cmake_consumer/main.c"
            -o "${ARCHBIRD_TEST_ROOT}/pkg-config-consumer"
            ${pkg_config_arguments}
    RESULT_VARIABLE pkg_config_compile_status
    OUTPUT_VARIABLE pkg_config_compile_stdout
    ERROR_VARIABLE pkg_config_compile_stderr)
  if(NOT pkg_config_compile_status EQUAL 0)
    message(FATAL_ERROR
            "pkg-config consumer failed to compile/link:\n"
            "${pkg_config_compile_stdout}\n${pkg_config_compile_stderr}")
  endif()
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -S "${ARCHBIRD_SOURCE_DIR}/test/cmake_consumer"
          -B "${consumer_build}"
          "-DCMAKE_PREFIX_PATH=${stage}"
          -DCMAKE_BUILD_TYPE=Release
  RESULT_VARIABLE configure_status
  OUTPUT_VARIABLE configure_stdout
  ERROR_VARIABLE configure_stderr)
if(NOT configure_status EQUAL 0)
  message(FATAL_ERROR
          "external consumer configure failed:\n${configure_stdout}\n${configure_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" ${config_args}
  RESULT_VARIABLE build_status
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr)
if(NOT build_status EQUAL 0)
  message(FATAL_ERROR
          "external consumer build failed:\n${build_stdout}\n${build_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${consumer_build}"
          --output-on-failure ${ctest_config_args}
  RESULT_VARIABLE test_status
  OUTPUT_VARIABLE test_stdout
  ERROR_VARIABLE test_stderr)
if(NOT test_status EQUAL 0)
  message(FATAL_ERROR
          "external consumer run failed:\n${test_stdout}\n${test_stderr}")
endif()
