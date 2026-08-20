cmake_minimum_required(VERSION 3.16)

project(Archbird VERSION 0.0.4 LANGUAGES C)

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

set(ARCHBIRD_CORE_SOURCE_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ArchbirdCoreSources.cmake")
if(NOT EXISTS "${ARCHBIRD_CORE_SOURCE_MANIFEST}")
  message(FATAL_ERROR
          "Native core source manifest is missing: "
          "${ARCHBIRD_CORE_SOURCE_MANIFEST}")
endif()
include("${ARCHBIRD_CORE_SOURCE_MANIFEST}")
if(NOT ARCHBIRD_DECLARED_CORE_SOURCE_PATHS)
  message(FATAL_ERROR "Native core source manifest contains no sources")
endif()
set(ARCHBIRD_SORTED_CORE_SOURCE_PATHS
    ${ARCHBIRD_DECLARED_CORE_SOURCE_PATHS})
list(SORT ARCHBIRD_SORTED_CORE_SOURCE_PATHS)
if(NOT "${ARCHBIRD_DECLARED_CORE_SOURCE_PATHS}" STREQUAL
       "${ARCHBIRD_SORTED_CORE_SOURCE_PATHS}")
  message(FATAL_ERROR
          "Native core source manifest must be sorted by source path")
endif()
set(ARCHBIRD_CORE_SOURCE_PATHS "")
set(ARCHBIRD_CORE_SOURCES "")
foreach(ARCHBIRD_CORE_SOURCE_PATH IN LISTS
        ARCHBIRD_DECLARED_CORE_SOURCE_PATHS)
  if(NOT ARCHBIRD_CORE_SOURCE_PATH MATCHES
     "^src/[A-Za-z0-9_./+-]+\\.c$" OR
     ARCHBIRD_CORE_SOURCE_PATH MATCHES "(^|/)\\.\\.(/|$)")
    message(FATAL_ERROR
            "Invalid native core source path: ${ARCHBIRD_CORE_SOURCE_PATH}")
  endif()
  list(FIND ARCHBIRD_CORE_SOURCE_PATHS "${ARCHBIRD_CORE_SOURCE_PATH}"
       ARCHBIRD_CORE_SOURCE_DUPLICATE_INDEX)
  if(NOT ARCHBIRD_CORE_SOURCE_DUPLICATE_INDEX EQUAL -1)
    message(FATAL_ERROR
            "Duplicate native core source path: ${ARCHBIRD_CORE_SOURCE_PATH}")
  endif()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_CORE_SOURCE_PATH}")
    message(FATAL_ERROR
            "Listed native core source does not exist: "
            "${ARCHBIRD_CORE_SOURCE_PATH}")
  endif()
  list(APPEND ARCHBIRD_CORE_SOURCE_PATHS "${ARCHBIRD_CORE_SOURCE_PATH}")
  list(APPEND ARCHBIRD_CORE_SOURCES
       "${CMAKE_CURRENT_SOURCE_DIR}/${ARCHBIRD_CORE_SOURCE_PATH}")
endforeach()
if(NOT ARCHBIRD_CORE_SOURCES)
  message(FATAL_ERROR "Native core source manifest contains no sources")
endif()
list(SORT ARCHBIRD_CORE_SOURCE_PATHS)
list(SORT ARCHBIRD_CORE_SOURCES)

set(ARCHBIRD_CORE_TARGET_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ArchbirdCoreTargets.cmake")
if(NOT EXISTS "${ARCHBIRD_CORE_TARGET_MANIFEST}")
  message(FATAL_ERROR
          "Native core target manifest is missing: "
          "${ARCHBIRD_CORE_TARGET_MANIFEST}")
endif()
include("${ARCHBIRD_CORE_TARGET_MANIFEST}")
if(NOT ARCHBIRD_DECLARED_CORE_GROUPS)
  message(FATAL_ERROR "Native core target manifest contains no groups")
endif()

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
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if(ARCHBIRD_TREE_SITTER_ENABLED)
  list(APPEND ARCHBIRD_INTERNAL_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/src"
    ${ARCHBIRD_TREE_SITTER_VENDOR_INCLUDE_DIRS}
  )
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ArchbirdCoreTargetBuild.cmake")

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
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson/src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/lib/include/*.h"
)
list(APPEND ARCHBIRD_IMPLEMENTATION_FILES
     ${ARCHBIRD_CORE_SOURCE_PATHS}
     ${ARCHBIRD_PCRE2_IDENTITY_FILES})
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

archbird_add_core_objects(static FALSE ARCHBIRD_STATIC_CORE_OBJECTS)
add_library(archbird_native STATIC
  ${ARCHBIRD_STATIC_CORE_OBJECTS}
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson/src/yyjson.c"
  $<TARGET_OBJECTS:archbird_pcre2>
)
archbird_configure_core_library(archbird_native FALSE)

# The static archive remains the self-contained link input for CPython,
# Node-API, Wasm, tests, and release packages.  The shared target exposes the
# same public C ABI for native consumers and source-checkout development as
# build/libarchbird.so (or the platform equivalent) without changing frontend
# artifact ownership.
if(ARCHBIRD_BUILD_SHARED)
  archbird_add_core_objects(shared TRUE ARCHBIRD_SHARED_CORE_OBJECTS)
  add_library(archbird_shared SHARED
    ${ARCHBIRD_SHARED_CORE_OBJECTS}
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson/src/yyjson.c"
    $<TARGET_OBJECTS:archbird_pcre2>
  )
  archbird_configure_core_library(archbird_shared TRUE)
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

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ArchbirdFuzzing.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ArchbirdTesting.cmake")

if(ARCHBIRD_BUILD_PYTHON)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
  COMMAND "${Python3_EXECUTABLE}" -c
          "import sysconfig; print(sysconfig.get_path('include'), end='')"
  RESULT_VARIABLE ARCHBIRD_PYTHON_INCLUDE_STATUS
  OUTPUT_VARIABLE ARCHBIRD_PYTHON_INCLUDE_DIR
  ERROR_VARIABLE ARCHBIRD_PYTHON_INCLUDE_ERROR
)
if(NOT ARCHBIRD_PYTHON_INCLUDE_STATUS EQUAL 0 OR
   NOT EXISTS "${ARCHBIRD_PYTHON_INCLUDE_DIR}/Python.h")
  message(FATAL_ERROR
    "The selected Python interpreter did not provide usable development "
    "headers: ${ARCHBIRD_PYTHON_INCLUDE_ERROR}"
  )
endif()
set(Python3_INCLUDE_DIR "${ARCHBIRD_PYTHON_INCLUDE_DIR}" CACHE PATH
    "Python headers paired with Python3_EXECUTABLE" FORCE)
find_package(Python3 REQUIRED COMPONENTS Development.Module)
if(Python3_Development.Module_FOUND)
  Python3_add_library(archbird_python MODULE bindings/python.c WITH_SOABI)
  target_link_libraries(archbird_python PRIVATE archbird_native)
  target_include_directories(archbird_python PRIVATE src)
  target_compile_definitions(archbird_python PRIVATE
    ARCHBIRD_CONFIGURED_PYTHON_MAJOR=${Python3_VERSION_MAJOR}
    ARCHBIRD_CONFIGURED_PYTHON_MINOR=${Python3_VERSION_MINOR}
  )
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
    add_test(
      NAME archbird_native_compile_context_scaling
      COMMAND ${CMAKE_COMMAND} -E env
              ${ARCHBIRD_REPOSITORY_TEST_ENV}
              ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_compile_context_scaling.py
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
      NAME archbird_native_freshness
      COMMAND ${CMAKE_COMMAND} -E env
              ${ARCHBIRD_REPOSITORY_TEST_ENV}
              ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_freshness.py
              $<TARGET_FILE:archbird_python>
    )
    add_test(
      NAME archbird_native_path
      COMMAND ${CMAKE_COMMAND} -E env
              ${ARCHBIRD_REPOSITORY_TEST_ENV}
              ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_path.py
              $<TARGET_FILE:archbird_python>
    )
    add_test(
      NAME archbird_native_path_evaluation
      COMMAND ${CMAKE_COMMAND} -E env
              ${ARCHBIRD_REPOSITORY_TEST_ENV}
              ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_path_evaluation.py
              $<TARGET_FILE:archbird_python>
    )
    add_test(
      NAME archbird_native_query_evidence_routes
      COMMAND ${CMAKE_COMMAND} -E env
              ${ARCHBIRD_REPOSITORY_TEST_ENV}
              ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_query_evidence_routes.py
              $<TARGET_FILE:archbird_python>
    )
    add_test(
      NAME archbird_native_query_adjacency_scaling
      COMMAND ${CMAKE_COMMAND} -E env
              ${ARCHBIRD_REPOSITORY_TEST_ENV}
              ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/test_query_adjacency_scaling.py
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
find_program(ARCHBIRD_NODE_EXECUTABLE node)
set(ARCHBIRD_NODE_API_HINTS
  /usr/local/include/node
  /usr/include/node
  "$ENV{NODE_INCLUDE_DIR}"
)
if(ARCHBIRD_NODE_EXECUTABLE)
  get_filename_component(ARCHBIRD_NODE_EXECUTABLE_REAL
    "${ARCHBIRD_NODE_EXECUTABLE}" REALPATH
  )
  get_filename_component(ARCHBIRD_NODE_BIN_DIR
    "${ARCHBIRD_NODE_EXECUTABLE_REAL}" DIRECTORY
  )
  get_filename_component(ARCHBIRD_NODE_PREFIX
    "${ARCHBIRD_NODE_BIN_DIR}" DIRECTORY
  )
  list(APPEND ARCHBIRD_NODE_API_HINTS
    "${ARCHBIRD_NODE_PREFIX}/include/node"
  )
endif()
find_path(ARCHBIRD_NODE_API_INCLUDE_DIR node_api.h
  HINTS ${ARCHBIRD_NODE_API_HINTS}
)
if(NOT ARCHBIRD_NODE_API_INCLUDE_DIR)
  message(FATAL_ERROR
    "ARCHBIRD_BUILD_NODE=ON requires Node-API headers; install Node development "
    "headers or set NODE_INCLUDE_DIR"
  )
endif()
  add_library(archbird_node MODULE bindings/node.c)
  target_link_libraries(archbird_node PRIVATE archbird_native)
  target_include_directories(archbird_node PRIVATE
    src ${ARCHBIRD_NODE_API_INCLUDE_DIR}
  )
  target_compile_definitions(archbird_node PRIVATE
    NAPI_VERSION=8 NODE_GYP_MODULE_NAME=_native
  )
  if(ARCHBIRD_WARNINGS_AS_ERRORS AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(archbird_node PRIVATE -fanalyzer)
  endif()
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

if(BUILD_TESTING)
  # Every registered test belongs to exactly one runtime class. The complete
  # native-test gate still runs both classes; the labels provide explicit
  # rapid-feedback and exhaustive-allocation/differential routes.
  set(ARCHBIRD_CTEST_SOAK_TESTS
    archbird_native_allocator
    archbird_project_configuration_differential)
  get_property(ARCHBIRD_CTEST_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
  if(NOT ARCHBIRD_CTEST_REGISTERED_TESTS)
    message(FATAL_ERROR "BUILD_TESTING registered no Archbird tests")
  endif()
  archbird_apply_sanitizer_test_environment()
  foreach(ARCHBIRD_CTEST_TEST IN LISTS ARCHBIRD_CTEST_REGISTERED_TESTS)
    list(FIND ARCHBIRD_CTEST_SOAK_TESTS "${ARCHBIRD_CTEST_TEST}"
         ARCHBIRD_CTEST_SOAK_INDEX)
    if(ARCHBIRD_CTEST_SOAK_INDEX EQUAL -1)
      set(ARCHBIRD_CTEST_CLASS fast)
    else()
      set(ARCHBIRD_CTEST_CLASS soak)
    endif()
    set_property(TEST "${ARCHBIRD_CTEST_TEST}" PROPERTY LABELS
                 "${ARCHBIRD_CTEST_CLASS}")
  endforeach()
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
