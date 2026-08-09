include_guard(GLOBAL)

if(NOT ARCHBIRD_DECLARED_CORE_GROUPS)
  message(FATAL_ERROR "Native core target manifest contains no groups")
endif()
if(NOT ARCHBIRD_CORE_SOURCES)
  message(FATAL_ERROR "Native core target build has no selected sources")
endif()
list(FIND ARCHBIRD_DECLARED_CORE_GROUPS "${ARCHBIRD_CORE_TREE_SITTER_GROUP}"
     ARCHBIRD_CORE_TREE_SITTER_GROUP_INDEX)
if(ARCHBIRD_CORE_TREE_SITTER_GROUP_INDEX EQUAL -1)
  message(FATAL_ERROR
          "Tree-sitter native sources must name one declared core group")
endif()

# Partition the selected source set into dependency-ordered coarse targets.
# Project component ownership remains in archbird.json; the target manifest
# only maps physical src/ directory prefixes to compiler targets.
foreach(ARCHBIRD_CORE_GROUP IN LISTS ARCHBIRD_DECLARED_CORE_GROUPS)
  set("ARCHBIRD_CORE_GROUP_${ARCHBIRD_CORE_GROUP}_SOURCES" "")
endforeach()
foreach(ARCHBIRD_CORE_SOURCE IN LISTS ARCHBIRD_CORE_SOURCES)
  file(RELATIVE_PATH ARCHBIRD_CORE_SOURCE_RELATIVE
       "${CMAKE_CURRENT_SOURCE_DIR}/src" "${ARCHBIRD_CORE_SOURCE}")
  set(ARCHBIRD_CORE_SOURCE_MATCHES "")
  if(ARCHBIRD_CORE_SOURCE_RELATIVE MATCHES "^\\.\\./vendor/tree-sitter")
    list(APPEND ARCHBIRD_CORE_SOURCE_MATCHES
         "${ARCHBIRD_CORE_TREE_SITTER_GROUP}")
  elseif(ARCHBIRD_CORE_SOURCE_RELATIVE MATCHES "^\\.\\./")
    message(FATAL_ERROR
            "Native core source is outside src/ and has no target: "
            "${ARCHBIRD_CORE_SOURCE}")
  else()
    foreach(ARCHBIRD_CORE_GROUP IN LISTS ARCHBIRD_DECLARED_CORE_GROUPS)
      set(ARCHBIRD_CORE_PREFIX_VARIABLE
          "ARCHBIRD_CORE_GROUP_${ARCHBIRD_CORE_GROUP}_SOURCE_PREFIXES")
      foreach(ARCHBIRD_CORE_PREFIX IN LISTS ${ARCHBIRD_CORE_PREFIX_VARIABLE})
        string(FIND
          "${ARCHBIRD_CORE_SOURCE_RELATIVE}/"
          "${ARCHBIRD_CORE_PREFIX}/"
          ARCHBIRD_CORE_PREFIX_INDEX)
        if(ARCHBIRD_CORE_PREFIX_INDEX EQUAL 0)
          list(APPEND ARCHBIRD_CORE_SOURCE_MATCHES
               "${ARCHBIRD_CORE_GROUP}")
        endif()
      endforeach()
    endforeach()
  endif()
  list(LENGTH ARCHBIRD_CORE_SOURCE_MATCHES ARCHBIRD_CORE_SOURCE_MATCH_COUNT)
  if(NOT ARCHBIRD_CORE_SOURCE_MATCH_COUNT EQUAL 1)
    message(FATAL_ERROR
            "Native core source must match exactly one coarse target: "
            "${ARCHBIRD_CORE_SOURCE} matched "
            "${ARCHBIRD_CORE_SOURCE_MATCHES}")
  endif()
  list(GET ARCHBIRD_CORE_SOURCE_MATCHES 0 ARCHBIRD_CORE_SOURCE_GROUP)
  list(APPEND "ARCHBIRD_CORE_GROUP_${ARCHBIRD_CORE_SOURCE_GROUP}_SOURCES"
       "${ARCHBIRD_CORE_SOURCE}")
endforeach()

# Each target compiles against a generated header view containing only its own
# headers and the transitive closure of its declared target dependencies.
# COPYONLY is portable to native and Wasm toolchains; the content-addressed
# root prevents removed headers or edges from surviving a reconfiguration.
file(GLOB_RECURSE ARCHBIRD_PRIVATE_HEADER_PATHS
  RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/src" CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
list(SORT ARCHBIRD_PRIVATE_HEADER_PATHS)
set(ARCHBIRD_CORE_TARGET_TOPOLOGY_MATERIAL
    "groups=${ARCHBIRD_DECLARED_CORE_GROUPS}\n"
    "headers=${ARCHBIRD_PRIVATE_HEADER_PATHS}\n")
set(ARCHBIRD_CORE_PROCESSED_GROUPS "")
foreach(ARCHBIRD_CORE_GROUP IN LISTS ARCHBIRD_DECLARED_CORE_GROUPS)
  set(ARCHBIRD_CORE_PREFIX_VARIABLE
      "ARCHBIRD_CORE_GROUP_${ARCHBIRD_CORE_GROUP}_SOURCE_PREFIXES")
  set(ARCHBIRD_CORE_DEPENDENCY_VARIABLE
      "ARCHBIRD_CORE_GROUP_${ARCHBIRD_CORE_GROUP}_DEPENDENCIES")
  set(ARCHBIRD_CORE_VISIBLE_GROUPS "${ARCHBIRD_CORE_GROUP}")
  foreach(ARCHBIRD_CORE_DEPENDENCY IN LISTS
          ${ARCHBIRD_CORE_DEPENDENCY_VARIABLE})
    list(FIND ARCHBIRD_CORE_PROCESSED_GROUPS
         "${ARCHBIRD_CORE_DEPENDENCY}" ARCHBIRD_CORE_DEPENDENCY_INDEX)
    if(ARCHBIRD_CORE_DEPENDENCY_INDEX EQUAL -1)
      message(FATAL_ERROR
              "Native core group ${ARCHBIRD_CORE_GROUP} depends on unknown or "
              "later group ${ARCHBIRD_CORE_DEPENDENCY}")
    endif()
    list(APPEND ARCHBIRD_CORE_VISIBLE_GROUPS
         "${ARCHBIRD_CORE_DEPENDENCY}"
         ${ARCHBIRD_CORE_GROUP_${ARCHBIRD_CORE_DEPENDENCY}_VISIBLE_GROUPS})
  endforeach()
  list(REMOVE_DUPLICATES ARCHBIRD_CORE_VISIBLE_GROUPS)
  set("ARCHBIRD_CORE_GROUP_${ARCHBIRD_CORE_GROUP}_VISIBLE_GROUPS"
      "${ARCHBIRD_CORE_VISIBLE_GROUPS}")
  string(APPEND ARCHBIRD_CORE_TARGET_TOPOLOGY_MATERIAL
         "${ARCHBIRD_CORE_GROUP}:"
         "${${ARCHBIRD_CORE_PREFIX_VARIABLE}}:"
         "${${ARCHBIRD_CORE_DEPENDENCY_VARIABLE}}\n")
  list(APPEND ARCHBIRD_CORE_PROCESSED_GROUPS "${ARCHBIRD_CORE_GROUP}")
endforeach()
string(SHA256 ARCHBIRD_CORE_TARGET_TOPOLOGY_SHA256
       "${ARCHBIRD_CORE_TARGET_TOPOLOGY_MATERIAL}")
set(ARCHBIRD_CORE_PRIVATE_INCLUDE_BASE
    "${CMAKE_CURRENT_BINARY_DIR}/archbird-private-include/${ARCHBIRD_CORE_TARGET_TOPOLOGY_SHA256}")

foreach(ARCHBIRD_CORE_GROUP IN LISTS ARCHBIRD_DECLARED_CORE_GROUPS)
  set(ARCHBIRD_CORE_PRIVATE_INCLUDE_ROOT
      "${ARCHBIRD_CORE_PRIVATE_INCLUDE_BASE}/${ARCHBIRD_CORE_GROUP}")
  foreach(ARCHBIRD_PRIVATE_HEADER IN LISTS ARCHBIRD_PRIVATE_HEADER_PATHS)
    set(ARCHBIRD_PRIVATE_HEADER_VISIBLE OFF)
    foreach(ARCHBIRD_CORE_VISIBLE_GROUP IN LISTS
            ARCHBIRD_CORE_GROUP_${ARCHBIRD_CORE_GROUP}_VISIBLE_GROUPS)
      set(ARCHBIRD_CORE_PREFIX_VARIABLE
          "ARCHBIRD_CORE_GROUP_${ARCHBIRD_CORE_VISIBLE_GROUP}_SOURCE_PREFIXES")
      foreach(ARCHBIRD_CORE_PREFIX IN LISTS ${ARCHBIRD_CORE_PREFIX_VARIABLE})
        string(FIND
          "${ARCHBIRD_PRIVATE_HEADER}/"
          "${ARCHBIRD_CORE_PREFIX}/"
          ARCHBIRD_CORE_PREFIX_INDEX)
        if(ARCHBIRD_CORE_PREFIX_INDEX EQUAL 0)
          set(ARCHBIRD_PRIVATE_HEADER_VISIBLE ON)
        endif()
      endforeach()
    endforeach()
    if(ARCHBIRD_PRIVATE_HEADER_VISIBLE)
      configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/${ARCHBIRD_PRIVATE_HEADER}"
        "${ARCHBIRD_CORE_PRIVATE_INCLUDE_ROOT}/${ARCHBIRD_PRIVATE_HEADER}"
        COPYONLY)
    endif()
  endforeach()
  set("ARCHBIRD_CORE_GROUP_${ARCHBIRD_CORE_GROUP}_PRIVATE_INCLUDE_ROOT"
      "${ARCHBIRD_CORE_PRIVATE_INCLUDE_ROOT}")
endforeach()

function(archbird_apply_core_compile_contract target shared_library)
  target_compile_definitions(${target} PRIVATE
    ARCHBIRD_LEXICAL_C_IMPLEMENTATION_SHA256="${ARCHBIRD_LEXICAL_C_IMPLEMENTATION_SHA256}"
    ARCHBIRD_LEXICAL_JAVASCRIPT_IMPLEMENTATION_SHA256="${ARCHBIRD_LEXICAL_JAVASCRIPT_IMPLEMENTATION_SHA256}"
    ARCHBIRD_LEXICAL_PYTHON_IMPLEMENTATION_SHA256="${ARCHBIRD_LEXICAL_PYTHON_IMPLEMENTATION_SHA256}"
    ARCHBIRD_LEXICAL_R_IMPLEMENTATION_SHA256="${ARCHBIRD_LEXICAL_R_IMPLEMENTATION_SHA256}"
    ARCHBIRD_SCIP_IMPLEMENTATION_SHA256="${ARCHBIRD_SCIP_IMPLEMENTATION_SHA256}"
    ARCHBIRD_IMPLEMENTATION_SHA256="${ARCHBIRD_IMPLEMENTATION_SHA256}"
    ARCHBIRD_VERSION="${PROJECT_VERSION}"
    TREE_SITTER_HIDE_SYMBOLS=1
    TREE_SITTER_HIDDEN_SYMBOLS=1
    yyjson_api=)
  if(shared_library)
    target_compile_definitions(${target} PRIVATE ARCHBIRD_SHARED_BUILD=1)
  endif()
  if(ARCHBIRD_TREE_SITTER_ENABLED)
    target_compile_definitions(${target} PRIVATE
      TREE_SITTER_REUSE_ALLOCATOR=1)
    foreach(ARCHBIRD_TREE_SITTER_PACK IN ITEMS C CPP PYTHON JAVASCRIPT TYPESCRIPT TSX R)
      if(ARCHBIRD_ENABLE_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK})
        target_compile_definitions(${target} PRIVATE
          ARCHBIRD_HAVE_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK}=1
          ARCHBIRD_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK}_IMPLEMENTATION_SHA256="${ARCHBIRD_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK}_IMPLEMENTATION_SHA256}")
      endif()
    endforeach()
    if(UNIX AND NOT APPLE)
      target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE=1)
    endif()
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow)
  endif()
  if(ARCHBIRD_ENABLE_SANITIZERS)
    target_compile_options(${target} PRIVATE
      -fno-omit-frame-pointer -fsanitize=address,undefined)
  endif()
  if(ARCHBIRD_ENABLE_FUZZERS)
    target_compile_options(${target} PRIVATE
      -fno-omit-frame-pointer -fsanitize=fuzzer-no-link,address,undefined)
  endif()
endfunction()

function(archbird_configure_core_object target group shared_library)
  set_target_properties(${target} PROPERTIES
    C_EXTENSIONS OFF
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON)
  target_compile_features(${target} PRIVATE c_std_11)
  target_include_directories(${target} PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    ${ARCHBIRD_CORE_GROUP_${group}_PRIVATE_INCLUDE_ROOT}
    ${ARCHBIRD_TREE_SITTER_VENDOR_INCLUDE_DIRS}
    $<$<BOOL:${ARCHBIRD_TREE_SITTER_ENABLED}>:${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/include>
    $<$<BOOL:${ARCHBIRD_TREE_SITTER_ENABLED}>:${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/src>
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson/src
    ${ARCHBIRD_PCRE2_GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/pcre2/src)
  archbird_apply_core_compile_contract(${target} ${shared_library})
endfunction()

function(archbird_add_core_objects variant shared_library output_variable)
  set(ARCHBIRD_CORE_OBJECTS "")
  foreach(ARCHBIRD_CORE_GROUP IN LISTS ARCHBIRD_DECLARED_CORE_GROUPS)
    set(ARCHBIRD_CORE_OBJECT_TARGET
        "archbird_core_${variant}_${ARCHBIRD_CORE_GROUP}")
    set(ARCHBIRD_CORE_GROUP_SOURCE_VARIABLE
        "ARCHBIRD_CORE_GROUP_${ARCHBIRD_CORE_GROUP}_SOURCES")
    list(LENGTH ${ARCHBIRD_CORE_GROUP_SOURCE_VARIABLE}
         ARCHBIRD_CORE_GROUP_SOURCE_COUNT)
    if(ARCHBIRD_CORE_GROUP_SOURCE_COUNT EQUAL 0)
      message(FATAL_ERROR
              "Native core group ${ARCHBIRD_CORE_GROUP} has no selected sources")
    endif()
    add_library(${ARCHBIRD_CORE_OBJECT_TARGET} OBJECT
      ${${ARCHBIRD_CORE_GROUP_SOURCE_VARIABLE}})
    archbird_configure_core_object(
      ${ARCHBIRD_CORE_OBJECT_TARGET} ${ARCHBIRD_CORE_GROUP} ${shared_library})
    list(APPEND ARCHBIRD_CORE_OBJECTS
         $<TARGET_OBJECTS:${ARCHBIRD_CORE_OBJECT_TARGET}>)
  endforeach()
  set(${output_variable} "${ARCHBIRD_CORE_OBJECTS}" PARENT_SCOPE)
endfunction()

function(archbird_configure_core_library target shared_library)
  set_target_properties(${target} PROPERTIES
    C_EXTENSIONS OFF
    C_VISIBILITY_PRESET hidden
    OUTPUT_NAME archbird
    POSITION_INDEPENDENT_CODE ON)
  target_compile_features(${target} PUBLIC c_std_11)
  target_include_directories(${target}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson/src
      ${ARCHBIRD_PCRE2_GENERATED_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/vendor/pcre2/src)
  archbird_apply_core_compile_contract(${target} ${shared_library})
  if(shared_library)
    target_compile_definitions(${target} INTERFACE ARCHBIRD_SHARED_USE=1)
  endif()
  if(ARCHBIRD_ENABLE_SANITIZERS)
    if(shared_library)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    else()
      # Instrumented static objects require every final consumer to link the
      # sanitizer runtime. Propagate both halves of that contract instead of
      # maintaining a brittle list of in-tree executables.
      target_compile_options(${target} INTERFACE
        -fno-omit-frame-pointer -fsanitize=address,undefined)
      target_link_options(${target} INTERFACE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()
