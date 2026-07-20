cmake_minimum_required(VERSION 3.16)

project(Archbird VERSION 0.0.1 LANGUAGES C)

set(ARCHBIRD_SOVERSION 0)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/archbird/archbird.h"
     ARCHBIRD_ABI_VERSION_DEFINITION
     REGEX "^#define ARCHBIRD_NATIVE_ABI_VERSION [0-9]+u$")
string(REGEX REPLACE ".* ([0-9]+)u$" "\\1" ARCHBIRD_HEADER_ABI_VERSION
       "${ARCHBIRD_ABI_VERSION_DEFINITION}")
if(NOT ARCHBIRD_HEADER_ABI_VERSION STREQUAL "${ARCHBIRD_SOVERSION}")
  message(FATAL_ERROR
          "ARCHBIRD_SOVERSION=${ARCHBIRD_SOVERSION} does not match "
          "ARCHBIRD_NATIVE_ABI_VERSION=${ARCHBIRD_HEADER_ABI_VERSION}")
endif()

include(CTest)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(ARCHBIRD_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/Archbird"
    CACHE STRING "Install directory for Archbird CMake package files")

function(archbird_require_sha256 variable)
  set(value "${${variable}}")
  string(LENGTH "${value}" length)
  if(NOT length EQUAL 64 OR NOT value MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
            "${variable} must contain exactly 64 lowercase hexadecimal characters")
  endif()
endfunction()

option(ARCHBIRD_ENABLE_SANITIZERS "Enable address and undefined sanitizers" OFF)
option(ARCHBIRD_ENABLE_FUZZERS "Build Clang/libFuzzer hardening targets" OFF)
option(ARCHBIRD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ARCHBIRD_BUILD_PYTHON "Build the CPython extension when available" OFF)
option(ARCHBIRD_BUILD_NODE "Build the Node-API extension when available" OFF)
option(ARCHBIRD_BUILD_SHARED "Build the generic shared C ABI library" OFF)
option(ARCHBIRD_ENABLE_TREE_SITTER
       "Build the portable Tree-sitter syntax runtime" ON)
foreach(ARCHBIRD_TREE_SITTER_PACK IN ITEMS C CPP PYTHON JAVASCRIPT TYPESCRIPT TSX R)
  option(ARCHBIRD_ENABLE_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK}
         "Build the portable Tree-sitter ${ARCHBIRD_TREE_SITTER_PACK} syntax provider" ON)
  if(NOT ARCHBIRD_ENABLE_TREE_SITTER)
    set(ARCHBIRD_ENABLE_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK} OFF)
  endif()
endforeach()
set(ARCHBIRD_REQUIRED_VENDOR_FILES
    vendor/yyjson/src/yyjson.c
    vendor/pcre2/src/pcre2_compile.c)
if(ARCHBIRD_ENABLE_TREE_SITTER)
  list(APPEND ARCHBIRD_REQUIRED_VENDOR_FILES vendor/tree-sitter/lib/src/lib.c)
endif()
if(ARCHBIRD_ENABLE_TREE_SITTER_C)
  list(APPEND ARCHBIRD_REQUIRED_VENDOR_FILES vendor/tree-sitter-c/src/parser.c)
endif()
if(ARCHBIRD_ENABLE_TREE_SITTER_CPP)
  list(APPEND ARCHBIRD_REQUIRED_VENDOR_FILES vendor/tree-sitter-cpp/src/parser.c)
endif()
if(ARCHBIRD_ENABLE_TREE_SITTER_PYTHON)
  list(APPEND ARCHBIRD_REQUIRED_VENDOR_FILES vendor/tree-sitter-python/src/parser.c)
endif()
if(ARCHBIRD_ENABLE_TREE_SITTER_JAVASCRIPT)
  list(APPEND ARCHBIRD_REQUIRED_VENDOR_FILES
       vendor/tree-sitter-javascript/src/parser.c)
endif()
if(ARCHBIRD_ENABLE_TREE_SITTER_TYPESCRIPT)
  list(APPEND ARCHBIRD_REQUIRED_VENDOR_FILES
       vendor/tree-sitter-typescript/typescript/src/parser.c)
endif()
if(ARCHBIRD_ENABLE_TREE_SITTER_TSX)
  list(APPEND ARCHBIRD_REQUIRED_VENDOR_FILES
       vendor/tree-sitter-typescript/tsx/src/parser.c)
endif()
if(ARCHBIRD_ENABLE_TREE_SITTER_R)
  list(APPEND ARCHBIRD_REQUIRED_VENDOR_FILES vendor/tree-sitter-r/src/parser.c)
endif()
set(ARCHBIRD_MISSING_VENDOR_FILES "")
foreach(ARCHBIRD_VENDOR_FILE IN LISTS ARCHBIRD_REQUIRED_VENDOR_FILES)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_VENDOR_FILE}")
    list(APPEND ARCHBIRD_MISSING_VENDOR_FILES "${ARCHBIRD_VENDOR_FILE}")
  endif()
endforeach()
if(ARCHBIRD_MISSING_VENDOR_FILES)
  list(JOIN ARCHBIRD_MISSING_VENDOR_FILES ", " ARCHBIRD_MISSING_VENDOR_TEXT)
  message(FATAL_ERROR
          "Pinned dependency submodules are not initialized: "
          "${ARCHBIRD_MISSING_VENDOR_TEXT}. Run: git submodule update --init "
          "--recursive --depth 1")
endif()
if(ARCHBIRD_ENABLE_SANITIZERS AND ARCHBIRD_ENABLE_FUZZERS)
  message(FATAL_ERROR
          "ARCHBIRD_ENABLE_SANITIZERS and ARCHBIRD_ENABLE_FUZZERS are separate build modes")
endif()
if(ARCHBIRD_ENABLE_FUZZERS AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "ARCHBIRD_ENABLE_FUZZERS requires Clang/libFuzzer")
endif()
if(ARCHBIRD_WARNINGS_AS_ERRORS)
  if(MSVC)
    add_compile_options(/WX)
  else()
    add_compile_options(-Werror)
  endif()
endif()
set(ARCHBIRD_REFERENCE_ROOT "" CACHE PATH
    "Optional maintainer reference checkout used by differential tests")
if(NOT ARCHBIRD_REFERENCE_ROOT AND DEFINED ENV{ARCHBIRD_REFERENCE_ROOT})
  set(ARCHBIRD_REFERENCE_ROOT "$ENV{ARCHBIRD_REFERENCE_ROOT}" CACHE PATH
      "Optional maintainer reference checkout used by differential tests" FORCE)
endif()
set(ARCHBIRD_OKF_PYTHON "" CACHE FILEPATH
    "Python interpreter with PyYAML/markdown-it-py for OKF differentials")
if(NOT ARCHBIRD_OKF_PYTHON AND DEFINED ENV{ARCHBIRD_OKF_PYTHON})
  set(ARCHBIRD_OKF_PYTHON "$ENV{ARCHBIRD_OKF_PYTHON}" CACHE FILEPATH
      "Python interpreter with PyYAML/markdown-it-py for OKF differentials"
      FORCE)
endif()

set(ARCHBIRD_PCRE2_SOURCE_NAMES
  pcre2_auto_possess.c
  pcre2_chkdint.c
  pcre2_compile.c
  pcre2_compile_cgroup.c
  pcre2_compile_class.c
  pcre2_config.c
  pcre2_context.c
  pcre2_convert.c
  pcre2_dfa_match.c
  pcre2_error.c
  pcre2_extuni.c
  pcre2_find_bracket.c
  pcre2_jit_compile.c
  pcre2_maketables.c
  pcre2_match.c
  pcre2_match_data.c
  pcre2_match_next.c
  pcre2_newline.c
  pcre2_ord2utf.c
  pcre2_pattern_info.c
  pcre2_script_run.c
  pcre2_serialize.c
  pcre2_string_utils.c
  pcre2_study.c
  pcre2_substitute.c
  pcre2_substring.c
  pcre2_tables.c
  pcre2_ucd.c
  pcre2_valid_utf.c
  pcre2_xclass.c)
set(ARCHBIRD_PCRE2_SOURCES "")
set(ARCHBIRD_PCRE2_IDENTITY_FILES
  vendor/pcre2/src/config.h.generic
  vendor/pcre2/src/pcre2.h.generic
  vendor/pcre2/src/pcre2_chartables.c.dist
  vendor/pcre2/src/pcre2_compile.h
  vendor/pcre2/src/pcre2_internal.h
  vendor/pcre2/src/pcre2_intmodedep.h
  vendor/pcre2/src/pcre2_jit_char_inc.h
  vendor/pcre2/src/pcre2_jit_match_inc.h
  vendor/pcre2/src/pcre2_jit_misc_inc.h
  vendor/pcre2/src/pcre2_jit_simd_inc.h
  vendor/pcre2/src/pcre2_ucp.h
  vendor/pcre2/src/pcre2_ucptables_inc.h
  vendor/pcre2/src/pcre2_util.h)
foreach(ARCHBIRD_PCRE2_SOURCE_NAME IN LISTS ARCHBIRD_PCRE2_SOURCE_NAMES)
  list(APPEND ARCHBIRD_PCRE2_SOURCES
       "${CMAKE_CURRENT_SOURCE_DIR}/vendor/pcre2/src/${ARCHBIRD_PCRE2_SOURCE_NAME}")
  list(APPEND ARCHBIRD_PCRE2_IDENTITY_FILES
       "vendor/pcre2/src/${ARCHBIRD_PCRE2_SOURCE_NAME}")
endforeach()
list(REMOVE_DUPLICATES ARCHBIRD_PCRE2_IDENTITY_FILES)
list(SORT ARCHBIRD_PCRE2_IDENTITY_FILES)

set(ARCHBIRD_PCRE2_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/vendor/pcre2")
file(MAKE_DIRECTORY "${ARCHBIRD_PCRE2_GENERATED_DIR}")
configure_file(vendor/pcre2/src/config.h.generic
               "${ARCHBIRD_PCRE2_GENERATED_DIR}/config.h" COPYONLY)
configure_file(vendor/pcre2/src/pcre2.h.generic
               "${ARCHBIRD_PCRE2_GENERATED_DIR}/pcre2.h" COPYONLY)
configure_file(vendor/pcre2/src/pcre2_chartables.c.dist
               "${ARCHBIRD_PCRE2_GENERATED_DIR}/pcre2_chartables.c" COPYONLY)
list(APPEND ARCHBIRD_PCRE2_SOURCES
     "${ARCHBIRD_PCRE2_GENERATED_DIR}/pcre2_chartables.c")

add_library(archbird_pcre2 OBJECT ${ARCHBIRD_PCRE2_SOURCES})
set_target_properties(archbird_pcre2 PROPERTIES
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON
  C_EXTENSIONS OFF
)
target_compile_features(archbird_pcre2 PRIVATE c_std_11)
target_include_directories(archbird_pcre2 PRIVATE
  "${ARCHBIRD_PCRE2_GENERATED_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/pcre2/src")
target_compile_definitions(archbird_pcre2 PRIVATE
  HAVE_CONFIG_H PCRE2_CODE_UNIT_WIDTH=8 PCRE2_STATIC SUPPORT_PCRE2_8
  SUPPORT_UNICODE
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  # Upstream's generated error-message table intentionally exceeds the C99
  # minimum required string-literal length; supported compilers accept it.
  target_compile_options(archbird_pcre2 PRIVATE -Wno-overlength-strings)
endif()

file(GLOB_RECURSE ARCHBIRD_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)
list(SORT ARCHBIRD_CORE_SOURCES)
set(ARCHBIRD_TREE_SITTER_ENABLED OFF)
set(ARCHBIRD_TREE_SITTER_VENDOR_SOURCES "")
set(ARCHBIRD_TREE_SITTER_VENDOR_INCLUDE_DIRS "")
set(ARCHBIRD_TREE_SITTER_VENDOR_IDENTITY_DIRS "")
set(ARCHBIRD_TREE_SITTER_GENERATED_SOURCES "")
foreach(ARCHBIRD_TREE_SITTER_PACK IN ITEMS C CPP PYTHON JAVASCRIPT TYPESCRIPT TSX R)
  string(TOLOWER "${ARCHBIRD_TREE_SITTER_PACK}" ARCHBIRD_TREE_SITTER_DIRECTORY)
  if(ARCHBIRD_ENABLE_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK})
    set(ARCHBIRD_TREE_SITTER_ENABLED ON)
    set(ARCHBIRD_TREE_SITTER_VENDOR_DIRECTORY
        "vendor/tree-sitter-${ARCHBIRD_TREE_SITTER_DIRECTORY}")
    set(ARCHBIRD_TREE_SITTER_GENERATED_DIRECTORY
        "${ARCHBIRD_TREE_SITTER_VENDOR_DIRECTORY}/src")
    if(ARCHBIRD_TREE_SITTER_PACK STREQUAL "TYPESCRIPT")
      set(ARCHBIRD_TREE_SITTER_VENDOR_DIRECTORY
          "vendor/tree-sitter-typescript")
      set(ARCHBIRD_TREE_SITTER_GENERATED_DIRECTORY
          "vendor/tree-sitter-typescript/typescript/src")
    elseif(ARCHBIRD_TREE_SITTER_PACK STREQUAL "TSX")
      set(ARCHBIRD_TREE_SITTER_VENDOR_DIRECTORY
          "vendor/tree-sitter-typescript")
      set(ARCHBIRD_TREE_SITTER_GENERATED_DIRECTORY
          "vendor/tree-sitter-typescript/tsx/src")
    endif()
    set(ARCHBIRD_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK}_VENDOR_DIRECTORY
        "${ARCHBIRD_TREE_SITTER_VENDOR_DIRECTORY}")
    set(ARCHBIRD_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK}_GENERATED_DIRECTORY
        "${ARCHBIRD_TREE_SITTER_GENERATED_DIRECTORY}")
    list(APPEND ARCHBIRD_TREE_SITTER_VENDOR_SOURCES
      "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_TREE_SITTER_GENERATED_DIRECTORY}/parser.c")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_TREE_SITTER_GENERATED_DIRECTORY}/scanner.c")
      list(APPEND ARCHBIRD_TREE_SITTER_VENDOR_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_TREE_SITTER_GENERATED_DIRECTORY}/scanner.c")
    endif()
    list(APPEND ARCHBIRD_TREE_SITTER_VENDOR_INCLUDE_DIRS
      "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_TREE_SITTER_VENDOR_DIRECTORY}"
      "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_TREE_SITTER_GENERATED_DIRECTORY}")
    list(APPEND ARCHBIRD_TREE_SITTER_VENDOR_IDENTITY_DIRS
      "${ARCHBIRD_TREE_SITTER_GENERATED_DIRECTORY}")
  else()
    if(NOT (ARCHBIRD_TREE_SITTER_PACK STREQUAL "TYPESCRIPT" AND
            ARCHBIRD_ENABLE_TREE_SITTER_TSX))
      list(FILTER ARCHBIRD_CORE_SOURCES EXCLUDE REGEX
           "/src/evidence/syntax/tree_sitter/${ARCHBIRD_TREE_SITTER_DIRECTORY}/")
    endif()
  endif()
endforeach()
if(NOT ARCHBIRD_ENABLE_TREE_SITTER_JAVASCRIPT AND
   NOT ARCHBIRD_ENABLE_TREE_SITTER_TYPESCRIPT AND
   NOT ARCHBIRD_ENABLE_TREE_SITTER_TSX)
  list(FILTER ARCHBIRD_CORE_SOURCES EXCLUDE REGEX
       "/src/evidence/syntax/tree_sitter/ecmascript/")
endif()
if(ARCHBIRD_TREE_SITTER_ENABLED)
  list(APPEND ARCHBIRD_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/src/lib.c"
    ${ARCHBIRD_TREE_SITTER_VENDOR_SOURCES}
  )
else()
  list(FILTER ARCHBIRD_CORE_SOURCES EXCLUDE REGEX
       "/src/evidence/syntax/tree_sitter/")
endif()

set(ARCHBIRD_INTERNAL_INCLUDE_DIRS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/api"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/base"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/map"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/verify"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/act"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/interchange/graph"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/interchange/okf"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/interchange/reports"
)
if(ARCHBIRD_TREE_SITTER_ENABLED)
  list(APPEND ARCHBIRD_INTERNAL_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/syntax/tree_sitter"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/src"
    ${ARCHBIRD_TREE_SITTER_VENDOR_INCLUDE_DIRS}
  )
endif()

add_library(archbird_native STATIC
  ${ARCHBIRD_CORE_SOURCES}
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson/src/yyjson.c"
  $<TARGET_OBJECTS:archbird_pcre2>
)

file(STRINGS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/lexical/provider_identity.sources"
  ARCHBIRD_LEXICAL_COMMON_FILES
)
set(ARCHBIRD_LEXICAL_COMMON_MATERIAL "")
foreach(ARCHBIRD_LEXICAL_COMMON_FILE IN LISTS ARCHBIRD_LEXICAL_COMMON_FILES)
  file(SHA256
       "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_LEXICAL_COMMON_FILE}"
       ARCHBIRD_LEXICAL_COMMON_FILE_SHA256)
  string(APPEND ARCHBIRD_LEXICAL_COMMON_MATERIAL
         "${ARCHBIRD_LEXICAL_COMMON_FILE_SHA256}")
endforeach()
file(GLOB_RECURSE ARCHBIRD_PYTHON_SYNTAX_IDENTITY_FILES
  RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/python/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/python/*.h")
list(SORT ARCHBIRD_PYTHON_SYNTAX_IDENTITY_FILES)
set(ARCHBIRD_PYTHON_SYNTAX_IDENTITY_MATERIAL "")
foreach(ARCHBIRD_PYTHON_SYNTAX_IDENTITY_FILE IN LISTS
        ARCHBIRD_PYTHON_SYNTAX_IDENTITY_FILES)
  file(SHA256
       "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_PYTHON_SYNTAX_IDENTITY_FILE}"
       ARCHBIRD_PYTHON_SYNTAX_IDENTITY_SHA256)
  string(APPEND ARCHBIRD_PYTHON_SYNTAX_IDENTITY_MATERIAL
         "${ARCHBIRD_PYTHON_SYNTAX_IDENTITY_SHA256}")
endforeach()
foreach(ARCHBIRD_LEXICAL_LANGUAGE IN ITEMS C JAVASCRIPT PYTHON R)
  string(TOLOWER "${ARCHBIRD_LEXICAL_LANGUAGE}" ARCHBIRD_LEXICAL_DIRECTORY)
  file(SHA256
       "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/lexical/${ARCHBIRD_LEXICAL_DIRECTORY}/scanner.c"
       ARCHBIRD_LEXICAL_LANGUAGE_SOURCE_SHA256)
  file(SHA256
       "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/lexical/${ARCHBIRD_LEXICAL_DIRECTORY}/scanner.h"
       ARCHBIRD_LEXICAL_LANGUAGE_HEADER_SHA256)
  set(ARCHBIRD_LEXICAL_LANGUAGE_EXTRA_MATERIAL "")
  if(ARCHBIRD_LEXICAL_LANGUAGE STREQUAL "PYTHON")
    set(ARCHBIRD_LEXICAL_LANGUAGE_EXTRA_MATERIAL
        "${ARCHBIRD_PYTHON_SYNTAX_IDENTITY_MATERIAL}")
  endif()
  string(SHA256 ARCHBIRD_LEXICAL_${ARCHBIRD_LEXICAL_LANGUAGE}_IMPLEMENTATION_SHA256
         "${ARCHBIRD_LEXICAL_COMMON_MATERIAL}${ARCHBIRD_LEXICAL_LANGUAGE_SOURCE_SHA256}${ARCHBIRD_LEXICAL_LANGUAGE_HEADER_SHA256}${ARCHBIRD_LEXICAL_LANGUAGE_EXTRA_MATERIAL}")
endforeach()
file(GLOB_RECURSE ARCHBIRD_SCIP_IDENTITY_FILES
  RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/semantic/scip/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/semantic/scip/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/fact_builder.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/fact_builder.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/base/model.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/base/model.h")
list(SORT ARCHBIRD_SCIP_IDENTITY_FILES)
set(ARCHBIRD_SCIP_IDENTITY_MATERIAL "")
foreach(ARCHBIRD_SCIP_IDENTITY_FILE IN LISTS ARCHBIRD_SCIP_IDENTITY_FILES)
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_SCIP_IDENTITY_FILE}"
       ARCHBIRD_SCIP_IDENTITY_SHA256)
  string(APPEND ARCHBIRD_SCIP_IDENTITY_MATERIAL
         "${ARCHBIRD_SCIP_IDENTITY_FILE}:${ARCHBIRD_SCIP_IDENTITY_SHA256}\n")
endforeach()
string(SHA256 ARCHBIRD_SCIP_IMPLEMENTATION_SHA256
       "${ARCHBIRD_SCIP_IDENTITY_MATERIAL}")
if(ARCHBIRD_TREE_SITTER_ENABLED)
  set(ARCHBIRD_TREE_SITTER_COMMON_IDENTITY_FILES
    src/evidence/syntax/tree_sitter/scanner.c
    src/evidence/syntax/tree_sitter/scanner.h
    src/evidence/syntax/tree_sitter/tree_sitter_allocator.c
    src/evidence/syntax/tree_sitter/tree_sitter_allocator.h
    src/evidence/fact_builder.c
    src/evidence/fact_builder.h
    src/base/model.c
    src/base/model.h)
  file(GLOB_RECURSE ARCHBIRD_TREE_SITTER_RUNTIME_IDENTITY_FILES
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/include/*.h")
  list(APPEND ARCHBIRD_TREE_SITTER_COMMON_IDENTITY_FILES
       ${ARCHBIRD_TREE_SITTER_RUNTIME_IDENTITY_FILES})
  foreach(ARCHBIRD_TREE_SITTER_PACK IN ITEMS C CPP PYTHON JAVASCRIPT TYPESCRIPT TSX R)
    if(ARCHBIRD_ENABLE_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK})
      string(TOLOWER "${ARCHBIRD_TREE_SITTER_PACK}" ARCHBIRD_TREE_SITTER_DIRECTORY)
      set(ARCHBIRD_TREE_SITTER_PACK_GENERATED_DIRECTORY
          "${ARCHBIRD_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK}_GENERATED_DIRECTORY}")
      file(GLOB_RECURSE ARCHBIRD_TREE_SITTER_PACK_IDENTITY_FILES
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/syntax/tree_sitter/${ARCHBIRD_TREE_SITTER_DIRECTORY}/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/syntax/tree_sitter/${ARCHBIRD_TREE_SITTER_DIRECTORY}/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_TREE_SITTER_PACK_GENERATED_DIRECTORY}/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_TREE_SITTER_PACK_GENERATED_DIRECTORY}/*.h")
      if(ARCHBIRD_TREE_SITTER_PACK STREQUAL "JAVASCRIPT" OR
         ARCHBIRD_TREE_SITTER_PACK STREQUAL "TYPESCRIPT" OR
         ARCHBIRD_TREE_SITTER_PACK STREQUAL "TSX")
        file(GLOB ARCHBIRD_TREE_SITTER_ECMASCRIPT_IDENTITY_FILES
          RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
          "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/syntax/tree_sitter/ecmascript/*.c"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/syntax/tree_sitter/ecmascript/*.h")
        list(APPEND ARCHBIRD_TREE_SITTER_PACK_IDENTITY_FILES
             ${ARCHBIRD_TREE_SITTER_ECMASCRIPT_IDENTITY_FILES})
      endif()
      if(ARCHBIRD_TREE_SITTER_PACK STREQUAL "TYPESCRIPT" OR
         ARCHBIRD_TREE_SITTER_PACK STREQUAL "TSX")
        list(APPEND ARCHBIRD_TREE_SITTER_PACK_IDENTITY_FILES
             vendor/tree-sitter-typescript/common/scanner.h)
      endif()
      if(ARCHBIRD_TREE_SITTER_PACK STREQUAL "PYTHON")
        list(APPEND ARCHBIRD_TREE_SITTER_PACK_IDENTITY_FILES
             ${ARCHBIRD_PYTHON_SYNTAX_IDENTITY_FILES})
      endif()
      if(ARCHBIRD_TREE_SITTER_PACK STREQUAL "TSX")
        file(GLOB ARCHBIRD_TREE_SITTER_TYPESCRIPT_IDENTITY_FILES
          RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
          "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/syntax/tree_sitter/typescript/*.c"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence/syntax/tree_sitter/typescript/*.h")
        list(APPEND ARCHBIRD_TREE_SITTER_PACK_IDENTITY_FILES
             ${ARCHBIRD_TREE_SITTER_TYPESCRIPT_IDENTITY_FILES})
      endif()
      set(ARCHBIRD_TREE_SITTER_IDENTITY_FILES
          ${ARCHBIRD_TREE_SITTER_COMMON_IDENTITY_FILES}
          ${ARCHBIRD_TREE_SITTER_PACK_IDENTITY_FILES})
      list(REMOVE_DUPLICATES ARCHBIRD_TREE_SITTER_IDENTITY_FILES)
      list(SORT ARCHBIRD_TREE_SITTER_IDENTITY_FILES)
      set(ARCHBIRD_TREE_SITTER_IDENTITY_MATERIAL "")
      foreach(ARCHBIRD_TREE_SITTER_IDENTITY_FILE IN LISTS
              ARCHBIRD_TREE_SITTER_IDENTITY_FILES)
        file(SHA256
             "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_TREE_SITTER_IDENTITY_FILE}"
             ARCHBIRD_TREE_SITTER_IDENTITY_SHA256)
        string(APPEND ARCHBIRD_TREE_SITTER_IDENTITY_MATERIAL
               "${ARCHBIRD_TREE_SITTER_IDENTITY_FILE}:${ARCHBIRD_TREE_SITTER_IDENTITY_SHA256}\n")
      endforeach()
      string(SHA256
        ARCHBIRD_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK}_IMPLEMENTATION_SHA256
        "${ARCHBIRD_TREE_SITTER_IDENTITY_MATERIAL}")
    endif()
  endforeach()
endif()
file(GLOB_RECURSE ARCHBIRD_IMPLEMENTATION_FILES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson/src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/include/*.h"
)
list(APPEND ARCHBIRD_IMPLEMENTATION_FILES ${ARCHBIRD_PCRE2_IDENTITY_FILES})
foreach(ARCHBIRD_TREE_SITTER_VENDOR_IDENTITY_DIR IN LISTS
        ARCHBIRD_TREE_SITTER_VENDOR_IDENTITY_DIRS)
  file(GLOB_RECURSE ARCHBIRD_TREE_SITTER_IMPLEMENTATION_FILES
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_TREE_SITTER_VENDOR_IDENTITY_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_TREE_SITTER_VENDOR_IDENTITY_DIR}/*.h")
  list(APPEND ARCHBIRD_IMPLEMENTATION_FILES
       ${ARCHBIRD_TREE_SITTER_IMPLEMENTATION_FILES})
endforeach()
if(ARCHBIRD_ENABLE_TREE_SITTER_TYPESCRIPT OR ARCHBIRD_ENABLE_TREE_SITTER_TSX)
  list(APPEND ARCHBIRD_IMPLEMENTATION_FILES
       vendor/tree-sitter-typescript/common/scanner.h)
endif()
list(REMOVE_DUPLICATES ARCHBIRD_IMPLEMENTATION_FILES)
list(SORT ARCHBIRD_IMPLEMENTATION_FILES)
set(ARCHBIRD_IMPLEMENTATION_MATERIAL "")
foreach(ARCHBIRD_IMPLEMENTATION_FILE IN LISTS ARCHBIRD_IMPLEMENTATION_FILES)
  set(ARCHBIRD_IMPLEMENTATION_PATH
      "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_IMPLEMENTATION_FILE}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${ARCHBIRD_IMPLEMENTATION_PATH}")
  file(SHA256 "${ARCHBIRD_IMPLEMENTATION_PATH}"
       ARCHBIRD_IMPLEMENTATION_FILE_SHA256)
  string(APPEND ARCHBIRD_IMPLEMENTATION_MATERIAL
         "${ARCHBIRD_IMPLEMENTATION_FILE}:${ARCHBIRD_IMPLEMENTATION_FILE_SHA256}\n")
endforeach()
string(SHA256 ARCHBIRD_IMPLEMENTATION_SHA256
       "${ARCHBIRD_IMPLEMENTATION_MATERIAL}")
archbird_require_sha256(ARCHBIRD_IMPLEMENTATION_SHA256)
foreach(ARCHBIRD_LEXICAL_LANGUAGE IN ITEMS C JAVASCRIPT PYTHON R)
  archbird_require_sha256(
    ARCHBIRD_LEXICAL_${ARCHBIRD_LEXICAL_LANGUAGE}_IMPLEMENTATION_SHA256)
endforeach()
archbird_require_sha256(ARCHBIRD_SCIP_IMPLEMENTATION_SHA256)
foreach(ARCHBIRD_TREE_SITTER_PACK IN ITEMS C CPP PYTHON JAVASCRIPT TYPESCRIPT TSX R)
  if(ARCHBIRD_ENABLE_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK})
    archbird_require_sha256(
      ARCHBIRD_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK}_IMPLEMENTATION_SHA256)
  endif()
endforeach()
if(ARCHBIRD_TREE_SITTER_ENABLED)
  if(MSVC)
    set_source_files_properties(
      vendor/tree-sitter/lib/src/lib.c ${ARCHBIRD_TREE_SITTER_VENDOR_SOURCES}
      PROPERTIES COMPILE_OPTIONS "/W0")
  else()
    set_source_files_properties(
      vendor/tree-sitter/lib/src/lib.c ${ARCHBIRD_TREE_SITTER_VENDOR_SOURCES}
      PROPERTIES COMPILE_OPTIONS "-w")
  endif()
endif()

function(archbird_configure_core_target target shared_library)
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
      ${ARCHBIRD_INTERNAL_INCLUDE_DIRS}
      ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson/src
      ${ARCHBIRD_PCRE2_GENERATED_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/vendor/pcre2/src)
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
    target_compile_definitions(${target} INTERFACE ARCHBIRD_SHARED_USE=1)
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
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(ARCHBIRD_ENABLE_SANITIZERS)
    target_compile_options(${target} PRIVATE
      -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
  if(ARCHBIRD_ENABLE_FUZZERS)
    target_compile_options(${target} PRIVATE
      -fno-omit-frame-pointer -fsanitize=fuzzer-no-link,address,undefined)
  endif()
endfunction()

if(ARCHBIRD_ENABLE_SANITIZERS)
  if(MSVC)
    message(FATAL_ERROR "ARCHBIRD_ENABLE_SANITIZERS is not configured for MSVC")
  endif()
  target_compile_options(archbird_pcre2 PRIVATE
    -fno-omit-frame-pointer -fsanitize=address,undefined)
endif()
if(ARCHBIRD_ENABLE_FUZZERS)
  target_compile_options(archbird_pcre2 PRIVATE
    -fno-omit-frame-pointer -fsanitize=fuzzer-no-link,address,undefined)
endif()

archbird_configure_core_target(archbird_native FALSE)

# The static archive remains the self-contained link input for CPython,
# Node-API, Wasm, tests, and release packages.  The shared target exposes the
# same public C ABI for native consumers and source-checkout development as
# build/libarchbird.so (or the platform equivalent) without changing frontend
# artifact ownership.
if(ARCHBIRD_BUILD_SHARED)
  add_library(archbird_shared SHARED
    ${ARCHBIRD_CORE_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson/src/yyjson.c"
    $<TARGET_OBJECTS:archbird_pcre2>
  )
  archbird_configure_core_target(archbird_shared TRUE)
  set_target_properties(archbird_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${ARCHBIRD_SOVERSION}"
  )
  set_target_properties(archbird_shared PROPERTIES EXPORT_NAME archbird)
  set_target_properties(archbird_native PROPERTIES EXPORT_NAME archbird_static)
  add_library(Archbird::archbird ALIAS archbird_shared)
  add_library(Archbird::archbird_static ALIAS archbird_native)
else()
  set_target_properties(archbird_native PROPERTIES EXPORT_NAME archbird)
  add_library(Archbird::archbird ALIAS archbird_native)
endif()

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
  archbird_add_fuzzer(archbird_fuzz_schemas test/fuzz/fuzz_schemas.c)
  archbird_add_fuzzer(archbird_fuzz_pattern test/fuzz/fuzz_pattern.c)
  archbird_add_fuzzer(archbird_fuzz_okf test/fuzz/fuzz_okf.c)
  archbird_add_fuzzer(archbird_fuzz_verify test/fuzz/fuzz_verify.c)
  archbird_add_fuzzer(archbird_fuzz_verification_input
                      test/fuzz/fuzz_verify.c)
  target_compile_definitions(archbird_fuzz_verification_input PRIVATE
    ARCHBIRD_FUZZ_VERIFICATION_INPUT=1)

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
  set(ARCHBIRD_FUZZ_SMOKE_OPTIONS
      -runs=256 -seed=1 -verbosity=0 -max_len=65536)
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
      COMMAND archbird_fuzz_syntax ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
              ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/syntax)
    set(ARCHBIRD_FUZZ_SYNTAX_DEPENDENCY archbird_fuzz_syntax)
  endif()

  foreach(mode IN ITEMS verification proposal review contract result)
    archbird_add_fuzzer(archbird_fuzz_act_${mode} test/fuzz/fuzz_act.c)
    string(TOUPPER ${mode} upper_mode)
    target_compile_definitions(archbird_fuzz_act_${mode} PRIVATE
      ARCHBIRD_FUZZ_ACT_${upper_mode}=1)
  endforeach()
  archbird_add_fuzzer(archbird_fuzz_act_after test/fuzz/fuzz_act.c)

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
    COMMAND archbird_fuzz_json ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/json
    COMMAND archbird_fuzz_schemas ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/schemas
    COMMAND archbird_fuzz_pattern ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/pattern
    COMMAND archbird_fuzz_okf ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/okf
    COMMAND archbird_fuzz_verify ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/verify
    COMMAND archbird_fuzz_verification_input ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/verification-input
    COMMAND archbird_fuzz_map ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/map
    COMMAND archbird_fuzz_query_input ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/query
    COMMAND archbird_fuzz_workspace ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/workspace
    COMMAND archbird_fuzz_workspace_maps ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/workspace-maps
    COMMAND archbird_fuzz_scip ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/scip
    ${ARCHBIRD_FUZZ_SYNTAX_COMMAND}
    COMMAND archbird_fuzz_act_verification ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/act-verification
    COMMAND archbird_fuzz_act_proposal ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/act-proposal
    COMMAND archbird_fuzz_act_review ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/act-review
    COMMAND archbird_fuzz_act_contract ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/act-contract
    COMMAND archbird_fuzz_act_after ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/act-after
    COMMAND archbird_fuzz_act_result ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/act-result
    COMMAND archbird_fuzz_lex_c ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/lex-c
    COMMAND archbird_fuzz_lex_javascript ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/lex-javascript
    COMMAND archbird_fuzz_lex_python ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/lex-python
    COMMAND archbird_fuzz_lex_r ${ARCHBIRD_FUZZ_SMOKE_OPTIONS}
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/lex-r
    DEPENDS archbird_fuzz_json archbird_fuzz_schemas archbird_fuzz_pattern
            archbird_fuzz_okf archbird_fuzz_verify
            archbird_fuzz_verification_input archbird_fuzz_map
            archbird_fuzz_query_input archbird_fuzz_workspace
            archbird_fuzz_workspace_maps archbird_fuzz_scip
            ${ARCHBIRD_FUZZ_SYNTAX_DEPENDENCY}
            archbird_fuzz_act_verification
            archbird_fuzz_act_proposal archbird_fuzz_act_review
            archbird_fuzz_act_contract archbird_fuzz_act_after
            archbird_fuzz_act_result archbird_fuzz_lex_c
            archbird_fuzz_lex_javascript archbird_fuzz_lex_python
            archbird_fuzz_lex_r
    USES_TERMINAL
  )
endif()

if(BUILD_TESTING)
  add_executable(archbird_native_json_test test/test_json.c)
  target_link_libraries(archbird_native_json_test PRIVATE archbird_native)
  target_compile_features(archbird_native_json_test PRIVATE c_std_11)
  set_target_properties(archbird_native_json_test PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_json test/json_cli.c)
  target_link_libraries(archbird_native_json PRIVATE archbird_native)
  target_compile_features(archbird_native_json PRIVATE c_std_11)
  set_target_properties(archbird_native_json PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_sha256_test test/test_sha256.c)
  target_link_libraries(archbird_native_sha256_test PRIVATE archbird_native)
  target_include_directories(archbird_native_sha256_test PRIVATE ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_features(archbird_native_sha256_test PRIVATE c_std_11)
  set_target_properties(archbird_native_sha256_test PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_hash test/hash_cli.c)
  target_link_libraries(archbird_native_hash PRIVATE archbird_native)
  target_include_directories(archbird_native_hash PRIVATE ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_features(archbird_native_hash PRIVATE c_std_11)
  set_target_properties(archbird_native_hash PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_project_test test/test_project.c)
  target_link_libraries(archbird_native_project_test PRIVATE archbird_native)
  target_compile_features(archbird_native_project_test PRIVATE c_std_11)
  set_target_properties(archbird_native_project_test PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_scip_test test/test_scip.c)
  target_link_libraries(archbird_native_scip_test PRIVATE archbird_native)
  target_include_directories(archbird_native_scip_test PRIVATE
    src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_features(archbird_native_scip_test PRIVATE c_std_11)
  set_target_properties(archbird_native_scip_test PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_lex_test test/test_lex.c)
  target_link_libraries(archbird_native_lex_test PRIVATE archbird_native)
  target_include_directories(archbird_native_lex_test PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_features(archbird_native_lex_test PRIVATE c_std_11)
  set_target_properties(archbird_native_lex_test PROPERTIES C_EXTENSIONS OFF)

  if(ARCHBIRD_TREE_SITTER_ENABLED)
    add_executable(archbird_native_tree_sitter_test test/test_tree_sitter.c)
    target_link_libraries(archbird_native_tree_sitter_test PRIVATE
                          archbird_native)
    target_include_directories(archbird_native_tree_sitter_test PRIVATE
      src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
    target_compile_features(archbird_native_tree_sitter_test PRIVATE c_std_11)
    foreach(ARCHBIRD_TREE_SITTER_PACK IN ITEMS C CPP PYTHON JAVASCRIPT TYPESCRIPT TSX R)
      if(ARCHBIRD_ENABLE_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK})
        target_compile_definitions(archbird_native_tree_sitter_test PRIVATE
          ARCHBIRD_TEST_TREE_SITTER_${ARCHBIRD_TREE_SITTER_PACK}=1)
      endif()
    endforeach()
    set_target_properties(archbird_native_tree_sitter_test PROPERTIES
      C_EXTENSIONS OFF)
  endif()

  add_executable(archbird_native_pattern_test test/test_pattern.c)
  target_link_libraries(archbird_native_pattern_test PRIVATE archbird_native)
  target_include_directories(archbird_native_pattern_test PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_features(archbird_native_pattern_test PRIVATE c_std_11)
  set_target_properties(archbird_native_pattern_test PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_gitignore_test test/test_gitignore.c)
  target_link_libraries(archbird_native_gitignore_test PRIVATE archbird_native)
  target_include_directories(archbird_native_gitignore_test PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_features(archbird_native_gitignore_test PRIVATE c_std_11)
  set_target_properties(archbird_native_gitignore_test PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_config_resolution_test test/test_config_resolution.c)
  target_link_libraries(archbird_native_config_resolution_test PRIVATE archbird_native)
  target_compile_features(archbird_native_config_resolution_test PRIVATE c_std_11)
  set_target_properties(archbird_native_config_resolution_test PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_allocator_test test/test_allocator.c)
  target_link_libraries(archbird_native_allocator_test PRIVATE archbird_native)
  target_include_directories(archbird_native_allocator_test PRIVATE
    src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_definitions(archbird_native_allocator_test PRIVATE
    "ARCHBIRD_ALLOCATOR_REPORT_MAP=\"${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/report_map.json\""
    "ARCHBIRD_ALLOCATOR_VERIFICATION=\"${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/map_base/query_overlay.verification.json\"")
  target_compile_features(archbird_native_allocator_test PRIVATE c_std_11)
  set_target_properties(archbird_native_allocator_test PROPERTIES
    C_EXTENSIONS OFF)

  add_executable(archbird_native_path_match_test test/test_path_match.c)
  target_link_libraries(archbird_native_path_match_test PRIVATE archbird_native)
  target_include_directories(archbird_native_path_match_test PRIVATE
    src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_features(archbird_native_path_match_test PRIVATE c_std_11)
  set_target_properties(archbird_native_path_match_test PROPERTIES
    C_EXTENSIONS OFF)

  add_executable(archbird_native_build_identity_test
    test/test_build_identity.c src/base/engine.c)
  target_include_directories(archbird_native_build_identity_test PRIVATE
    include src src/base)
  target_compile_features(archbird_native_build_identity_test PRIVATE c_std_11)
  target_compile_definitions(archbird_native_build_identity_test PRIVATE
    "ARCHBIRD_IMPLEMENTATION_SHA256=\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeq\"")
  set_target_properties(archbird_native_build_identity_test PROPERTIES
    C_EXTENSIONS OFF)

  add_executable(archbird_native_builds_test test/test_builds.c)
  target_link_libraries(archbird_native_builds_test PRIVATE archbird_native)
  target_include_directories(archbird_native_builds_test PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_features(archbird_native_builds_test PRIVATE c_std_11)
  set_target_properties(archbird_native_builds_test PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_pattern test/pattern_cli.c)
  target_link_libraries(archbird_native_pattern PRIVATE archbird_native)
  target_include_directories(archbird_native_pattern PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_features(archbird_native_pattern PRIVATE c_std_11)
  set_target_properties(archbird_native_pattern PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_lex test/lex_cli.c)
  target_link_libraries(archbird_native_lex PRIVATE archbird_native)
  target_include_directories(archbird_native_lex PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_features(archbird_native_lex PRIVATE c_std_11)
  set_target_properties(archbird_native_lex PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_scanner test/scanner_cli.c)
  target_link_libraries(archbird_native_scanner PRIVATE archbird_native)
  target_include_directories(archbird_native_scanner PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_features(archbird_native_scanner PRIVATE c_std_11)
  set_target_properties(archbird_native_scanner PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_verify test/verify_cli.c)
  target_link_libraries(archbird_native_verify PRIVATE archbird_native)
  target_include_directories(archbird_native_verify PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_features(archbird_native_verify PRIVATE c_std_11)
  set_target_properties(archbird_native_verify PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_verify_result test/verify_result_cli.c)
  target_link_libraries(archbird_native_verify_result PRIVATE archbird_native)
  target_include_directories(archbird_native_verify_result PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_features(archbird_native_verify_result PRIVATE c_std_11)
  set_target_properties(archbird_native_verify_result PROPERTIES
                        C_EXTENSIONS OFF)

  add_executable(archbird_native_act test/act_cli.c)
  target_link_libraries(archbird_native_act PRIVATE archbird_native)
  target_include_directories(archbird_native_act PRIVATE src ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
  target_compile_features(archbird_native_act PRIVATE c_std_11)
  set_target_properties(archbird_native_act PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_graph test/graph_cli.c)
  target_link_libraries(archbird_native_graph PRIVATE archbird_native)
  target_compile_features(archbird_native_graph PRIVATE c_std_11)
  set_target_properties(archbird_native_graph PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_okf test/okf_cli.c)
  target_link_libraries(archbird_native_okf PRIVATE archbird_native)
  target_compile_features(archbird_native_okf PRIVATE c_std_11)
  set_target_properties(archbird_native_okf PROPERTIES C_EXTENSIONS OFF)

  add_executable(archbird_native_okf_publish test/okf_publish_cli.c)
  target_link_libraries(archbird_native_okf_publish PRIVATE archbird_native)
  target_compile_features(archbird_native_okf_publish PRIVATE c_std_11)
  set_target_properties(archbird_native_okf_publish PROPERTIES C_EXTENSIONS OFF)

  if(ARCHBIRD_ENABLE_SANITIZERS)
    foreach(target archbird_native_json_test archbird_native_json
                   archbird_native_sha256_test archbird_native_hash
                   archbird_native_project_test archbird_native_lex_test
                   archbird_native_scip_test
                   archbird_native_pattern_test
                   archbird_native_gitignore_test
                   archbird_native_config_resolution_test
                   archbird_native_allocator_test
                   archbird_native_path_match_test
                   archbird_native_build_identity_test
                   archbird_native_builds_test
                   archbird_native_pattern
                   archbird_native_lex
                   archbird_native_scanner
                   archbird_native_verify
                   archbird_native_verify_result
                   archbird_native_act
                   archbird_native_graph
                   archbird_native_okf
                   archbird_native_okf_publish)
      target_compile_options(${target} PRIVATE
        -fno-omit-frame-pointer -fsanitize=address,undefined
      )
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endforeach()
    if(ARCHBIRD_TREE_SITTER_ENABLED)
      target_compile_options(archbird_native_tree_sitter_test PRIVATE
        -fno-omit-frame-pointer -fsanitize=address,undefined)
      target_link_options(archbird_native_tree_sitter_test PRIVATE
        -fsanitize=address,undefined)
    endif()
  endif()

  add_test(NAME archbird_native_json COMMAND archbird_native_json_test)
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
  add_test(NAME archbird_native_allocator COMMAND archbird_native_allocator_test)
  add_test(NAME archbird_native_path_match COMMAND archbird_native_path_match_test)
  add_test(NAME archbird_native_build_identity
           COMMAND archbird_native_build_identity_test)
  add_test(NAME archbird_native_builds COMMAND archbird_native_builds_test)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
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
        NAME archbird_native_okf_publish_reference
        COMMAND ${CMAKE_COMMAND} -E env
                PYTHONPATH=${ARCHBIRD_REFERENCE_ROOT}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_okf_publish_reference.py
                $<TARGET_FILE:archbird_native_okf_publish>
                ${ARCHBIRD_REFERENCE_ROOT}
                ${CMAKE_CURRENT_BINARY_DIR}
      )
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

if(ARCHBIRD_BUILD_PYTHON)
find_package(Python3 COMPONENTS Development.Module)
if(Python3_Development.Module_FOUND)
  Python3_add_library(archbird_python MODULE bindings/python.c WITH_SOABI)
  target_link_libraries(archbird_python PRIVATE archbird_native)
  target_include_directories(archbird_python PRIVATE src)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # PyMethodDef type-erases METH_KEYWORDS functions as PyCFunction; the
    # required method-table cast is interpreted using ml_flags by CPython.
    target_compile_options(archbird_python PRIVATE -Wno-cast-function-type)
  endif()
  set_target_properties(archbird_python PROPERTIES
    OUTPUT_NAME "_native"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/python/archbird"
  )
  if(ARCHBIRD_ENABLE_SANITIZERS)
    target_compile_options(archbird_python PRIVATE
      -fno-omit-frame-pointer -fsanitize=address,undefined
    )
    target_link_options(archbird_python PRIVATE -fsanitize=address,undefined)
  endif()
  if(BUILD_TESTING AND Python3_EXECUTABLE)
    if(ARCHBIRD_REFERENCE_ROOT)
      set(ARCHBIRD_PY_TEST_ROOT "${ARCHBIRD_REFERENCE_ROOT}")
    else()
      set(ARCHBIRD_PY_TEST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    set(ARCHBIRD_PY_TEST_ENV "PYTHONPATH=${ARCHBIRD_PY_TEST_ROOT}")
    if(ARCHBIRD_ENABLE_SANITIZERS)
      execute_process(
        COMMAND ${CMAKE_C_COMPILER} -print-file-name=libasan.so
        OUTPUT_VARIABLE ARCHBIRD_ASAN_RUNTIME
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE ARCHBIRD_ASAN_RESULT
      )
      if(NOT ARCHBIRD_ASAN_RESULT EQUAL 0 OR
         NOT EXISTS "${ARCHBIRD_ASAN_RUNTIME}")
        message(FATAL_ERROR "compiler did not provide a loadable ASan runtime")
      endif()
      list(APPEND ARCHBIRD_PY_TEST_ENV
           "LD_PRELOAD=${ARCHBIRD_ASAN_RUNTIME}"
           "PYTHONMALLOC=malloc")
    endif()
    if(ARCHBIRD_REFERENCE_ROOT)
      add_test(
        NAME archbird_native_python_frontend
        COMMAND ${CMAKE_COMMAND} -E env
                ${ARCHBIRD_PY_TEST_ENV}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/py/tests/test_frontend.py
                $<TARGET_FILE:archbird_python>
                ${CMAKE_CURRENT_SOURCE_DIR}
      )
    endif()
    set(ARCHBIRD_REPOSITORY_TEST_ENV
        "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/py")
    if(ARCHBIRD_ENABLE_SANITIZERS)
      list(APPEND ARCHBIRD_REPOSITORY_TEST_ENV
           "LD_PRELOAD=${ARCHBIRD_ASAN_RUNTIME}"
           "PYTHONMALLOC=malloc")
    endif()
    add_test(
      NAME archbird_native_python_repository
      COMMAND ${CMAKE_COMMAND} -E env
              ${ARCHBIRD_REPOSITORY_TEST_ENV}
              ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/py/tests/test_repository.py
              $<TARGET_FILE:archbird_python>
              ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(
      NAME archbird_native_scip_python_host
      COMMAND ${CMAKE_COMMAND} -E env
              ${ARCHBIRD_REPOSITORY_TEST_ENV}
              ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_scip_python_host.py
              $<TARGET_FILE:archbird_python>
              ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(
      NAME archbird_native_map_correctness
      COMMAND ${CMAKE_COMMAND} -E env
              ${ARCHBIRD_REPOSITORY_TEST_ENV}
              ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_map_correctness.py
              $<TARGET_FILE:archbird_python>
              ${CMAKE_CURRENT_SOURCE_DIR}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/map_correctness
    )
    add_test(
      NAME archbird_native_compile_commands
      COMMAND ${CMAKE_COMMAND} -E env
              ${ARCHBIRD_REPOSITORY_TEST_ENV}
              ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_compile_commands.py
              $<TARGET_FILE:archbird_python>
    )
    if(ARCHBIRD_ENABLE_TREE_SITTER_JAVASCRIPT)
      add_test(
        NAME archbird_native_ecmascript_modules
        COMMAND ${CMAKE_COMMAND} -E env
                ${ARCHBIRD_REPOSITORY_TEST_ENV}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_ecmascript_modules.py
                $<TARGET_FILE:archbird_python>
      )
      add_test(
        NAME archbird_native_ecmascript_identity
        COMMAND ${CMAKE_COMMAND} -E env
                ${ARCHBIRD_REPOSITORY_TEST_ENV}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_ecmascript_identity.py
                $<TARGET_FILE:archbird_python>
      )
    endif()
    add_test(
      NAME archbird_native_verify_source_lock
      COMMAND ${CMAKE_COMMAND} -E env
              ${ARCHBIRD_REPOSITORY_TEST_ENV}
              ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_verify_source_lock.py
              $<TARGET_FILE:archbird_python>
    )
    add_test(
      NAME archbird_native_freshness
      COMMAND ${CMAKE_COMMAND} -E env
              ${ARCHBIRD_REPOSITORY_TEST_ENV}
              ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_freshness.py
              $<TARGET_FILE:archbird_python>
    )
    add_test(
      NAME archbird_native_fuzz_seeds
      COMMAND ${CMAKE_COMMAND} -E env
              ${ARCHBIRD_REPOSITORY_TEST_ENV}
              ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_fuzz_seeds.py
              $<TARGET_FILE:archbird_python>
              ${CMAKE_CURRENT_SOURCE_DIR}/test/fuzz/corpus
    )
    add_test(
      NAME archbird_self_host_policy
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_self_host_policy.py
    )
    if(ARCHBIRD_REFERENCE_ROOT)
      add_test(
        NAME archbird_native_map_base
        COMMAND ${CMAKE_COMMAND} -E env
                ${ARCHBIRD_PY_TEST_ENV}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_map_base.py
                $<TARGET_FILE:archbird_python>
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/map_base
      )
      add_test(
        NAME archbird_native_map_packages
        COMMAND ${CMAKE_COMMAND} -E env
                ${ARCHBIRD_PY_TEST_ENV}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_map_packages.py
                $<TARGET_FILE:archbird_python>
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/map_packages
      )
      add_test(
        NAME archbird_native_config_reference
        COMMAND ${CMAKE_COMMAND} -E env
                ${ARCHBIRD_PY_TEST_ENV}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_config_reference.py
                $<TARGET_FILE:archbird_python>
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_BINARY_DIR}/config-reference.json
      )
      add_test(
        NAME archbird_native_discovery_reference
        COMMAND ${CMAKE_COMMAND} -E env
                ${ARCHBIRD_PY_TEST_ENV}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_discovery.py
                $<TARGET_FILE:archbird_python>
                ${ARCHBIRD_REFERENCE_ROOT}/tests/fixtures/sample/archbird.json
                ${ARCHBIRD_REFERENCE_ROOT}/tests/fixtures/sample
      )
      add_test(
        NAME archbird_native_query_base
        COMMAND ${CMAKE_COMMAND} -E env
                ${ARCHBIRD_PY_TEST_ENV}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_query.py
                $<TARGET_FILE:archbird_python>
                ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/map_base
      )
      add_test(
        NAME archbird_native_query_packages
        COMMAND ${CMAKE_COMMAND} -E env
                ${ARCHBIRD_PY_TEST_ENV}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_query.py
                $<TARGET_FILE:archbird_python>
                ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/map_packages
      )
      add_test(
        NAME archbird_native_diff
        COMMAND ${CMAKE_COMMAND} -E env
                ${ARCHBIRD_PY_TEST_ENV}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_diff.py
                $<TARGET_FILE:archbird_python>
                ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/map_packages
      )
      add_test(
        NAME archbird_native_workspace
        COMMAND ${CMAKE_COMMAND} -E env
                ${ARCHBIRD_PY_TEST_ENV}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_workspace.py
                $<TARGET_FILE:archbird_python>
                ${ARCHBIRD_REFERENCE_ROOT}/tests/fixtures/workspace.json
      )
      add_test(
        NAME archbird_native_verify_config
        COMMAND ${CMAKE_COMMAND} -E env
                ${ARCHBIRD_PY_TEST_ENV}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_verify_config.py
                $<TARGET_FILE:archbird_python>
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/verification/verification.json
      )
      add_test(
        NAME archbird_native_verify_facts
        COMMAND ${CMAKE_COMMAND} -E env
                PYTHONPATH=${ARCHBIRD_REFERENCE_ROOT}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_verify_facts.py
                $<TARGET_FILE:archbird_native_verify>
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/verification
      )
      add_test(
        NAME archbird_native_verify_results
        COMMAND ${CMAKE_COMMAND} -E env
                PYTHONPATH=${ARCHBIRD_REFERENCE_ROOT}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_verify_results.py
                $<TARGET_FILE:archbird_native_verify_result>
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/verification
      )
      add_test(
        NAME archbird_native_act_results
        COMMAND ${CMAKE_COMMAND} -E env
                PYTHONPATH=${ARCHBIRD_REFERENCE_ROOT}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_act_results.py
                $<TARGET_FILE:archbird_native_act>
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/verification
      )
      add_test(
        NAME archbird_native_graph_exports
        COMMAND ${CMAKE_COMMAND} -E env
                PYTHONPATH=${ARCHBIRD_REFERENCE_ROOT}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_graph_exports.py
                $<TARGET_FILE:archbird_native_graph>
                ${ARCHBIRD_REFERENCE_ROOT}/tests/fixtures/sample/archbird.json
      )
    endif()
  endif()
endif()
endif()

if(ARCHBIRD_BUILD_NODE)
find_path(ARCHBIRD_NODE_API_INCLUDE_DIR node_api.h
  HINTS
    /usr/local/include/node
    /usr/include/node
    "$ENV{NODE_INCLUDE_DIR}"
)
if(ARCHBIRD_NODE_API_INCLUDE_DIR)
  add_library(archbird_node MODULE bindings/node.c)
  target_link_libraries(archbird_node PRIVATE archbird_native)
  target_include_directories(archbird_node PRIVATE
    src ${ARCHBIRD_NODE_API_INCLUDE_DIR}
  )
  target_compile_definitions(archbird_node PRIVATE
    NAPI_VERSION=8 NODE_GYP_MODULE_NAME=_native
  )
  set_target_properties(archbird_node PROPERTIES
    PREFIX ""
    SUFFIX ".node"
    OUTPUT_NAME "_native"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/node"
  )
  if(ARCHBIRD_ENABLE_SANITIZERS)
    target_compile_options(archbird_node PRIVATE
      -fno-omit-frame-pointer -fsanitize=address,undefined
    )
    target_link_options(archbird_node PRIVATE -fsanitize=address,undefined)
  endif()
  find_program(ARCHBIRD_NODE_EXECUTABLE node)
  if(BUILD_TESTING AND ARCHBIRD_NODE_EXECUTABLE)
    set(ARCHBIRD_NODE_TEST_ENV)
    if(ARCHBIRD_ENABLE_SANITIZERS)
      if(NOT ARCHBIRD_ASAN_RUNTIME)
        execute_process(
          COMMAND ${CMAKE_C_COMPILER} -print-file-name=libasan.so
          OUTPUT_VARIABLE ARCHBIRD_ASAN_RUNTIME
          OUTPUT_STRIP_TRAILING_WHITESPACE
          RESULT_VARIABLE ARCHBIRD_ASAN_RESULT
        )
      endif()
      if(NOT ARCHBIRD_ASAN_RESULT EQUAL 0 OR
         NOT EXISTS "${ARCHBIRD_ASAN_RUNTIME}")
        message(FATAL_ERROR "compiler did not provide a loadable ASan runtime")
      endif()
      list(APPEND ARCHBIRD_NODE_TEST_ENV
           "LD_PRELOAD=${ARCHBIRD_ASAN_RUNTIME}")
    endif()
    add_test(
      NAME archbird_native_node_frontend
      COMMAND ${CMAKE_COMMAND} -E env ${ARCHBIRD_NODE_TEST_ENV}
              ${ARCHBIRD_NODE_EXECUTABLE}
              --expose-gc
              ${CMAKE_CURRENT_SOURCE_DIR}/js/test/test_frontend.js
              $<TARGET_FILE:archbird_node>
              ${CMAKE_CURRENT_SOURCE_DIR}
    )
    if(Python3_EXECUTABLE AND TARGET archbird_python)
      add_test(
        NAME archbird_native_resolution_frontend_parity
        COMMAND ${CMAKE_COMMAND} -E env
                ${ARCHBIRD_REPOSITORY_TEST_ENV}
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/test_resolution_frontend_parity.py
                $<TARGET_FILE:archbird_python>
                ${ARCHBIRD_NODE_EXECUTABLE}
                $<TARGET_FILE:archbird_node>
                ${CMAKE_CURRENT_SOURCE_DIR}
      )
    endif()
  endif()
endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  function(archbird_add_wasm_host target output_name synchronous)
    add_executable(${target} bindings/wasm.c)
    target_link_libraries(${target} PRIVATE archbird_native)
    target_include_directories(${target} PRIVATE
      src src/base ${ARCHBIRD_INTERNAL_INCLUDE_DIRS})
    target_compile_features(${target} PRIVATE c_std_11)
    set_target_properties(${target} PROPERTIES
      C_EXTENSIONS OFF
      OUTPUT_NAME "${output_name}"
      SUFFIX ".js"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/wasm"
    )
    target_compile_definitions(${target} PRIVATE
      ARCHBIRD_VERSION="${PROJECT_VERSION}")
    target_link_options(${target} PRIVATE
      --no-entry
      -sALLOW_MEMORY_GROWTH=1
      -sFILESYSTEM=0
      -sSTACK_SIZE=1048576
      "-sEXPORTED_FUNCTIONS=['_malloc','_free']"
      "-sEXPORTED_RUNTIME_METHODS=['HEAPU8']"
    )
    if(synchronous)
      target_link_options(${target} PRIVATE
        -sENVIRONMENT=node
        -sSINGLE_FILE=1
        -sWASM_ASYNC_COMPILATION=0)
    else()
      target_link_options(${target} PRIVATE
        -sMODULARIZE=1
        -sEXPORT_NAME=createArchbirdModule
        -sENVIRONMENT=web,worker)
    endif()
  endfunction()

  archbird_add_wasm_host(archbird_wasm_async archbird FALSE)
  archbird_add_wasm_host(archbird_wasm_sync archbird.sync TRUE)
endif()

configure_package_config_file(
  cmake/ArchbirdConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/ArchbirdConfig.cmake"
  INSTALL_DESTINATION "${ARCHBIRD_INSTALL_CMAKEDIR}")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/ArchbirdConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
configure_file(cmake/archbird.pc.in
               "${CMAKE_CURRENT_BINARY_DIR}/archbird.pc" @ONLY)

set(ARCHBIRD_INSTALL_TARGETS archbird_native)
if(ARCHBIRD_BUILD_SHARED)
  list(APPEND ARCHBIRD_INSTALL_TARGETS archbird_shared)
endif()
install(TARGETS ${ARCHBIRD_INSTALL_TARGETS}
        EXPORT ArchbirdTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES include/archbird/archbird.h
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/archbird")
install(EXPORT ArchbirdTargets
        FILE ArchbirdTargets.cmake
        NAMESPACE Archbird::
        DESTINATION "${ARCHBIRD_INSTALL_CMAKEDIR}")
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ArchbirdConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ArchbirdConfigVersion.cmake"
        DESTINATION "${ARCHBIRD_INSTALL_CMAKEDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/archbird.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
