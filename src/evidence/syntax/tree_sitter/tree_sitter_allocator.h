#ifndef ARCHBIRD_TREE_SITTER_ALLOCATOR_H
#define ARCHBIRD_TREE_SITTER_ALLOCATOR_H

#include "archbird_internal.h"

#include <stddef.h>

typedef ArchbirdStatus (*AbTreeSitterOperation)(void *user_data);

ArchbirdStatus ab_tree_sitter_with_allocator(ArchbirdEngine *engine,
                                             size_t byte_limit,
                                             AbTreeSitterOperation operation,
                                             void *user_data,
                                             int *out_resource_limited);

void *ab_tree_sitter_malloc(size_t size);
void *ab_tree_sitter_calloc(size_t count, size_t size);
void *ab_tree_sitter_realloc(void *pointer, size_t size);
void ab_tree_sitter_free(void *pointer);

#endif
