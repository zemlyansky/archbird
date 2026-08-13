#ifndef ARCHBIRD_CMAKE_PROJECT_MANIFEST_H
#define ARCHBIRD_CMAKE_PROJECT_MANIFEST_H

#include "base/model.h"

typedef struct AbCmakeProjectMetadata {
  AbString name;
} AbCmakeProjectMetadata;

/* Extract one literal project() call from the lexical root of a top-level
 * CMakeLists.txt. Conditional/function/macro bodies, project-command
 * redefinitions, a root return before project, variable expansion, computed
 * versions, malformed nesting, and multiple root calls remain unresolved.
 * This reader never executes CMake. */
ArchbirdStatus ab_cmake_project_metadata(ArchbirdEngine *engine,
                                         const uint8_t *text, size_t length,
                                         AbCmakeProjectMetadata *out);
void ab_cmake_project_metadata_free(ArchbirdEngine *engine,
                                    AbCmakeProjectMetadata *metadata);

#endif
