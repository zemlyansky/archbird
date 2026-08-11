include_guard(GLOBAL)

function(archbird_add_native_test_program target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE archbird_native)
  target_compile_features(${target} PRIVATE c_std_11)
  set_target_properties(${target} PROPERTIES C_EXTENSIONS OFF)
endfunction()

function(archbird_apply_sanitizer_test_environment)
  if(NOT ARCHBIRD_ENABLE_SANITIZERS)
    return()
  endif()
  get_property(ARCHBIRD_SANITIZER_TESTS DIRECTORY PROPERTY TESTS)
  set_tests_properties(${ARCHBIRD_SANITIZER_TESTS} PROPERTIES
    ENVIRONMENT
      "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1:abort_on_error=1")
endfunction()

if(BUILD_TESTING)
  archbird_add_native_test_program(archbird_native_json_test test/test_json.c)

  archbird_add_native_test_program(archbird_native_json_pointer_edit_test
                                   test/test_json_pointer_edit.c)
  target_include_directories(archbird_native_json_pointer_edit_test
                             PRIVATE ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  archbird_add_native_test_program(archbird_native_plan_model_test
                                   test/test_plan_model.c)
  archbird_add_native_test_program(archbird_native_plan_compile_test
                                   test/test_plan_compile.c)
  target_include_directories(archbird_native_plan_compile_test
                             PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  archbird_add_native_test_program(archbird_native_act_model_test
                                   test/test_act_model.c)
  archbird_add_native_test_program(archbird_native_act_materialize_test
                                   test/test_act_materialize.c)
  target_include_directories(archbird_native_act_materialize_test PRIVATE
    src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  archbird_add_native_test_program(archbird_native_make_variable_token_edit_test
                                   test/test_make_variable_token_edit.c)
  target_include_directories(archbird_native_make_variable_token_edit_test
                             PRIVATE ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})

  archbird_add_native_test_program(archbird_native_json test/json_cli.c)

  archbird_add_native_test_program(archbird_native_sha256_test test/test_sha256.c)
  target_include_directories(archbird_native_sha256_test PRIVATE ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})

  archbird_add_native_test_program(archbird_native_hash test/hash_cli.c)
  target_include_directories(archbird_native_hash PRIVATE ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})

  archbird_add_native_test_program(archbird_native_project_test test/test_project.c)

  archbird_add_native_test_program(archbird_native_scip_test test/test_scip.c)
  target_include_directories(archbird_native_scip_test PRIVATE
    src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})

  archbird_add_native_test_program(archbird_native_lex_test test/test_lex.c)
  target_include_directories(archbird_native_lex_test PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})

  if(ARCHBIRD_TREE_SITTER_ENABLED)
    archbird_add_native_test_program(archbird_native_tree_sitter_test
                                     test/test_tree_sitter.c)
    target_include_directories(archbird_native_tree_sitter_test PRIVATE
      src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
    foreach(ARCHBIRD_TREE_SITTER_PACK IN ITEMS C CPP PYTHON JAVASCRIPT TYPESCRIPT TSX R)
      if(ARCHBIRD_ENABLE_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK})
        target_compile_definitions(archbird_native_tree_sitter_test PRIVATE
          ARCHBIRD_TEST_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK}=1)
      endif()
    endforeach()
  endif()

  archbird_add_native_test_program(archbird_native_pattern_test test/test_pattern.c)
  target_include_directories(archbird_native_pattern_test PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})

  archbird_add_native_test_program(archbird_native_gitignore_test test/test_gitignore.c)
  target_include_directories(archbird_native_gitignore_test PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})

  archbird_add_native_test_program(archbird_native_config_resolution_test
                                   test/test_config_resolution.c)

  archbird_add_native_test_program(archbird_native_project_configuration_test
                                   test/test_project_configuration.c)

  archbird_add_native_test_program(archbird_native_allocator_test test/test_allocator.c)
  target_include_directories(archbird_native_allocator_test PRIVATE
    src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_definitions(archbird_native_allocator_test PRIVATE
    "ARCHBIRD_ALLOCATOR_REPORT_MAP=\"${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/report_map.json\"")

  archbird_add_native_test_program(archbird_native_path_match_test test/test_path_match.c)
  target_include_directories(archbird_native_path_match_test PRIVATE
    src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})

  add_executable(archbird_native_build_identity_test
    test/test_build_identity.c src/base/engine.c)
  target_include_directories(archbird_native_build_identity_test PRIVATE
    include src src/base)
  target_compile_features(archbird_native_build_identity_test PRIVATE c_std_11)
  target_compile_definitions(archbird_native_build_identity_test PRIVATE
    "ARCHBIRD_IMPLEMENTATION_SHA256=\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeq\"")
  set_target_properties(archbird_native_build_identity_test PROPERTIES
    C_EXTENSIONS OFF)

  archbird_add_native_test_program(archbird_native_builds_test test/test_builds.c)
  target_include_directories(archbird_native_builds_test PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})

  archbird_add_native_test_program(archbird_native_pattern test/pattern_cli.c)
  target_include_directories(archbird_native_pattern PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})

  archbird_add_native_test_program(archbird_native_lex test/lex_cli.c)
  target_include_directories(archbird_native_lex PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})

  archbird_add_native_test_program(archbird_native_scanner test/scanner_cli.c)
  target_include_directories(archbird_native_scanner PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})

  archbird_add_native_test_program(archbird_native_unified_diff_test
                                   test/test_unified_diff.c)

  archbird_add_native_test_program(archbird_native_graph test/graph_cli.c)

  archbird_add_native_test_program(archbird_native_okf test/okf_cli.c)

  archbird_add_native_test_program(archbird_native_okf_publish
                                   test/okf_publish_cli.c)

  add_test(NAME archbird_native_json COMMAND archbird_native_json_test)
  add_test(NAME archbird_native_json_pointer_edit
           COMMAND archbird_native_json_pointer_edit_test)
  add_test(NAME archbird_native_plan_model
           COMMAND archbird_native_plan_model_test)
  add_test(NAME archbird_native_plan_compile
           COMMAND archbird_native_plan_compile_test)
  add_test(NAME archbird_native_act_model
           COMMAND archbird_native_act_model_test)
  add_test(NAME archbird_native_act_materialize
           COMMAND archbird_native_act_materialize_test)
  add_test(NAME archbird_native_make_variable_token_edit
           COMMAND archbird_native_make_variable_token_edit_test)
  add_test(NAME archbird_native_sha256 COMMAND archbird_native_sha256_test)
  add_test(NAME archbird_native_project COMMAND archbird_native_project_test)
  add_test(NAME archbird_native_scip COMMAND archbird_native_scip_test)
  add_test(NAME archbird_native_lex COMMAND archbird_native_lex_test)
  if(ARCHBIRD_TREE_SITTER_ENABLED)
    add_test(NAME archbird_native_tree_sitter
             COMMAND archbird_native_tree_sitter_test)
  endif()
  add_test(NAME archbird_native_pattern COMMAND archbird_native_pattern_test)
  add_test(NAME archbird_native_gitignore COMMAND archbird_native_gitignore_test)
  add_test(NAME archbird_native_config_resolution COMMAND archbird_native_config_resolution_test)
  add_test(NAME archbird_native_project_configuration
           COMMAND archbird_native_project_configuration_test)
  add_test(NAME archbird_native_allocator COMMAND archbird_native_allocator_test)
  add_test(NAME archbird_native_path_match COMMAND archbird_native_path_match_test)
  add_test(NAME archbird_native_build_identity
           COMMAND archbird_native_build_identity_test)
  add_test(NAME archbird_native_builds COMMAND archbird_native_builds_test)
  add_test(NAME archbird_native_unified_diff
           COMMAND archbird_native_unified_diff_test)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND AND TARGET archbird_shared)
    add_test(
      NAME archbird_project_configuration_differential
      COMMAND ${CMAKE_COMMAND} -E env
              "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/py"
              "ARCHBIRD_LIB=$<TARGET_FILE:archbird_shared>"
              ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_project_configuration.py
    )
  endif()
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME archbird_core_source_manifest
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_core_source_manifest.py
              ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(
      NAME archbird_private_include_boundaries
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_private_include_boundaries.py
              ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(
      NAME archbird_core_target_manifest
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_core_target_manifest.py
              ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(
      NAME archbird_native_evidence
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_evidence.py
              $<TARGET_FILE:archbird_native_json>
    )
    add_test(
      NAME archbird_native_json_numbers
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_json_numbers.py
              $<TARGET_FILE:archbird_native_json>
    )
    add_test(
      NAME archbird_native_merge_ledger
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_merge_ledger.py
              $<TARGET_FILE:archbird_native_project_test>
    )
    add_test(
      NAME archbird_native_json_boundary
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_json_boundary.py
    )
    add_test(
      NAME archbird_native_allocator_boundary
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_allocator_boundary.py
    )
    add_test(
      NAME archbird_native_planning_boundary
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_planning_boundaries.py
    )
    add_test(
      NAME archbird_native_file_facts
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_file_facts.py
              $<TARGET_FILE:archbird_native_scanner>
    )
    add_test(
      NAME archbird_native_pattern_reference
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_pattern_reference.py
              $<TARGET_FILE:archbird_native_pattern>
    )
    add_test(
      NAME archbird_native_okf_policy
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_okf_policy.py
              $<TARGET_FILE:archbird_native_okf>
              ${CMAKE_CURRENT_BINARY_DIR}
    )
    if(ARCHBIRD_REFERENCE_ROOT AND
       ARCHBIRD_OKF_PYTHON AND EXISTS "${ARCHBIRD_OKF_PYTHON}")
      add_test(
        NAME archbird_native_okf_reference
        COMMAND ${ARCHBIRD_OKF_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_okf_reference.py
                $<TARGET_FILE:archbird_native_okf>
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${ARCHBIRD_REFERENCE_ROOT}
                ${CMAKE_CURRENT_BINARY_DIR}
      )
    endif()
    add_test(
      NAME archbird_native_python_scanner
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_python_scanner.py
              $<TARGET_FILE:archbird_native_scanner>
    )
    if(ARCHBIRD_REFERENCE_ROOT)
      add_test(
        NAME archbird_native_lex_reference
        COMMAND ${CMAKE_COMMAND} -E env
                PYTHONPATH=${ARCHBIRD_REFERENCE_ROOT}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_lex_reference.py
                $<TARGET_FILE:archbird_native_lex>
      )
      add_test(
        NAME archbird_native_c_scanner_reference
        COMMAND ${CMAKE_COMMAND} -E env
                PYTHONPATH=${ARCHBIRD_REFERENCE_ROOT}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_c_scanner_reference.py
                $<TARGET_FILE:archbird_native_scanner>
      )
      add_test(
        NAME archbird_native_r_scanner_reference
        COMMAND ${CMAKE_COMMAND} -E env
                PYTHONPATH=${ARCHBIRD_REFERENCE_ROOT}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_r_scanner_reference.py
                $<TARGET_FILE:archbird_native_scanner>
      )
      add_test(
        NAME archbird_native_js_scanner_reference
        COMMAND ${CMAKE_COMMAND} -E env
                PYTHONPATH=${ARCHBIRD_REFERENCE_ROOT}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_js_scanner_reference.py
                $<TARGET_FILE:archbird_native_scanner>
      )
      add_test(
        NAME archbird_native_python_scanner_reference
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_python_scanner_reference.py
                $<TARGET_FILE:archbird_native_scanner>
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${ARCHBIRD_REFERENCE_ROOT}
      )
    endif()
  endif()
  if(TARGET archbird_shared)
    add_test(
      NAME archbird_native_install_consumer
      COMMAND ${CMAKE_COMMAND}
              -DARCHBIRD_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
              -DARCHBIRD_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
              -DARCHBIRD_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/install-consumer
              -DARCHBIRD_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}
              -DARCHBIRD_BUILD_CONFIG=$<CONFIG>
              -DARCHBIRD_C_COMPILER=${CMAKE_C_COMPILER}
              -DCMAKE_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/test/test_install_consumer.cmake)
    if(Python3_Interpreter_FOUND)
      add_test(
        NAME archbird_native_shared_exports
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_shared_exports.py
                ${CMAKE_CURRENT_SOURCE_DIR}/include/archbird/archbird.h
                $<TARGET_FILE:archbird_shared>)
      set_tests_properties(archbird_native_install_consumer PROPERTIES
                           DEPENDS archbird_native_shared_exports)
    endif()
  endif()
endif()
