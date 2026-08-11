include_guard(GLOBAL)

if(ARCHBIRD_ENABLE_FUZZERS)
  function(archbird_add_fuzzer target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE archbird_native)
    target_include_directories(${target} PRIVATE
      src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
    target_compile_features(${target} PRIVATE c_std_11)
    set_target_properties(${target} PROPERTIES C_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE
      -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined)
    target_link_options(${target} PRIVATE
      -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # Clang 14's ASan runtime can collide with randomized PIE layouts before
      # libFuzzer initializes. Fuzz executables are local hardening tools, so a
      # fixed ET_EXEC layout is both deterministic and avoids that false crash.
      target_link_options(${target} PRIVATE -no-pie)
    endif()
  endfunction()

  archbird_add_fuzzer(archbird_fuzz_json test/fuzz/fuzz_json.c)
  archbird_add_fuzzer(archbird_fuzz_json_pointer_edit
                      test/fuzz/fuzz_json_pointer_edit.c)
  archbird_add_fuzzer(archbird_fuzz_schemas test/fuzz/fuzz_schemas.c)
  archbird_add_fuzzer(archbird_fuzz_pattern test/fuzz/fuzz_pattern.c)
  archbird_add_fuzzer(archbird_fuzz_okf test/fuzz/fuzz_okf.c)
  archbird_add_fuzzer(archbird_fuzz_map test/fuzz/fuzz_map.c)
  archbird_add_fuzzer(archbird_fuzz_query_input test/fuzz/fuzz_map.c)
  target_compile_definitions(archbird_fuzz_query_input PRIVATE
    ARCHBIRD_FUZZ_QUERY_INPUT=1)
  archbird_add_fuzzer(archbird_fuzz_workspace test/fuzz/fuzz_workspace.c)
  archbird_add_fuzzer(archbird_fuzz_workspace_maps
                      test/fuzz/fuzz_workspace.c)
  target_compile_definitions(archbird_fuzz_workspace_maps PRIVATE
    ARCHBIRD_FUZZ_WORKSPACE_MAPS=1)
  archbird_add_fuzzer(archbird_fuzz_scip test/fuzz/fuzz_scip.c)
  # Each fuzz executable links the complete debug-instrumented core. Routine
  # NEW_FUNC reporting starts a fresh llvm-symbolizer for every executable and
  # can spend minutes loading identical debug data without increasing coverage.
  # Crash reports remain symbolized through ASan/UBSan.
  set(ARCHBIRD_FUZZ_SMOKE_OPTIONS
      -runs=256 -seed=1 -verbosity=0 -print_funcs=0 -max_len=65536)
  set(ARCHBIRD_FUZZ_RUN
      ${CMAKE_COMMAND} -E env
      UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
      ASAN_OPTIONS=halt_on_error=1:abort_on_error=1)
  set(ARCHBIRD_FUZZ_SYNTAX_COMMAND)
  set(ARCHBIRD_FUZZ_SYNTAX_DEPENDENCY)
  if(ARCHBIRD_TREE_SITTER_ENABLED)
    archbird_add_fuzzer(archbird_fuzz_syntax test/fuzz/fuzz_syntax.c)
    foreach(ARCHBIRD_TREE_SITTER_PACK IN ITEMS C CPP PYTHON JAVASCRIPT TYPESCRIPT TSX R)
      if(ARCHBIRD_ENABLE_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK})
        target_compile_definitions(archbird_fuzz_syntax PRIVATE
          ARCHBIRD_FUZZ_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK}=1)
      endif()
    endforeach()
    set(ARCHBIRD_FUZZ_SYNTAX_COMMAND
      COMMAND ${ARCHBIRD_FUZZ_RUN}
              $<TARGET_FILE:archbird_fuzz_syntax>
              ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
              ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/syntax)
    set(ARCHBIRD_FUZZ_SYNTAX_DEPENDENCY archbird_fuzz_syntax)
  endif()

  foreach(language IN ITEMS c javascript python r)
    archbird_add_fuzzer(archbird_fuzz_lex_${language}
                        test/fuzz/fuzz_lexical.c)
    target_compile_definitions(archbird_fuzz_lex_${language} PRIVATE
      ARCHBIRD_FUZZ_LANGUAGE="${language}"
      ARCHBIRD_FUZZ_PATH="input.${language}"
      ARCHBIRD_FUZZ_PROVIDER="lexical:${language}")
  endforeach()

  add_custom_target(archbird_fuzz_smoke
    COMMAND ${CMAKE_COMMAND} -E remove_directory
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/test/fuzz/corpus
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus
    COMMAND ${ARCHBIRD_FUZZ_RUN}
            $<TARGET_FILE:archbird_fuzz_json> ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/json
    COMMAND ${ARCHBIRD_FUZZ_RUN}
            $<TARGET_FILE:archbird_fuzz_json_pointer_edit>
            ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/json
    COMMAND ${ARCHBIRD_FUZZ_RUN}
            $<TARGET_FILE:archbird_fuzz_schemas>
            ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/schemas
    COMMAND ${ARCHBIRD_FUZZ_RUN}
            $<TARGET_FILE:archbird_fuzz_pattern> ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/pattern
    COMMAND ${ARCHBIRD_FUZZ_RUN}
            $<TARGET_FILE:archbird_fuzz_okf> ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/okf
    COMMAND ${ARCHBIRD_FUZZ_RUN}
            $<TARGET_FILE:archbird_fuzz_map> ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/map
    COMMAND ${ARCHBIRD_FUZZ_RUN}
            $<TARGET_FILE:archbird_fuzz_query_input>
            ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/query
    COMMAND ${ARCHBIRD_FUZZ_RUN}
            $<TARGET_FILE:archbird_fuzz_workspace>
            ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/workspace
    COMMAND ${ARCHBIRD_FUZZ_RUN}
            $<TARGET_FILE:archbird_fuzz_workspace_maps>
            ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/workspace-maps
    COMMAND ${ARCHBIRD_FUZZ_RUN}
            $<TARGET_FILE:archbird_fuzz_scip> ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/scip
    ${ARCHBIRD_FUZZ_SYNTAX_COMMAND}
    COMMAND ${ARCHBIRD_FUZZ_RUN}
            $<TARGET_FILE:archbird_fuzz_lex_c>
            ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/lex-c
    COMMAND ${ARCHBIRD_FUZZ_RUN}
            $<TARGET_FILE:archbird_fuzz_lex_javascript>
            ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/lex-javascript
    COMMAND ${ARCHBIRD_FUZZ_RUN}
            $<TARGET_FILE:archbird_fuzz_lex_python>
            ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/lex-python
    COMMAND ${ARCHBIRD_FUZZ_RUN}
            $<TARGET_FILE:archbird_fuzz_lex_r>
            ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/lex-r
    DEPENDS archbird_fuzz_json archbird_fuzz_json_pointer_edit
            archbird_fuzz_schemas archbird_fuzz_pattern
            archbird_fuzz_okf archbird_fuzz_map
            archbird_fuzz_query_input archbird_fuzz_workspace
            archbird_fuzz_workspace_maps archbird_fuzz_scip
            ${ARCHBIRD_FUZZ_SYNTAX_DEPENDENCY}
            archbird_fuzz_lex_c
            archbird_fuzz_lex_javascript archbird_fuzz_lex_python
            archbird_fuzz_lex_r
    USES_TERMINAL
  )
endif()
