#include "tree_sitter_allocator.h"

#include "tree_sitter/api.h"

#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define AB_TREE_SITTER_ALLOCATION_MAGIC UINT64_C(0x617263687473616c)

typedef union AbTreeSitterAllocation AbTreeSitterAllocation;

union AbTreeSitterAllocation {
  max_align_t alignment;
  struct {
    uint64_t magic;
    size_t size;
    AbTreeSitterAllocation *previous;
    AbTreeSitterAllocation *next;
  } value;
};

typedef struct AbTreeSitterAllocationScope {
  ArchbirdEngine *engine;
  AbTreeSitterAllocation *head;
  size_t bytes;
  size_t byte_limit;
  ArchbirdStatus failure;
  int trapped;
  jmp_buf recovery;
} AbTreeSitterAllocationScope;

static _Thread_local AbTreeSitterAllocationScope *active_scope;
static atomic_flag allocator_configuration_lock = ATOMIC_FLAG_INIT;
static atomic_bool allocator_configured = 0;

static void configure_tree_sitter_allocator(void) {
  if (atomic_load_explicit(&allocator_configured, memory_order_acquire))
    return;
  while (atomic_flag_test_and_set_explicit(&allocator_configuration_lock,
                                           memory_order_acquire)) {
  }
  if (!atomic_load_explicit(&allocator_configured, memory_order_relaxed)) {
    ts_set_allocator(ab_tree_sitter_malloc, ab_tree_sitter_calloc,
                     ab_tree_sitter_realloc, ab_tree_sitter_free);
    atomic_store_explicit(&allocator_configured, 1, memory_order_release);
  }
  atomic_flag_clear_explicit(&allocator_configuration_lock,
                             memory_order_release);
}

static void allocation_fail(ArchbirdStatus status) {
  if (!active_scope)
    return;
  active_scope->failure = status;
  active_scope->trapped = 1;
  longjmp(active_scope->recovery, 1);
}

static int allocation_size(size_t payload, size_t *out_total) {
  payload = payload ? payload : 1;
  if (payload > SIZE_MAX - sizeof(AbTreeSitterAllocation))
    return 0;
  *out_total = sizeof(AbTreeSitterAllocation) + payload;
  return 1;
}

void *ab_tree_sitter_malloc(size_t size) {
  AbTreeSitterAllocation *allocation;
  size_t payload = size ? size : 1;
  size_t total;
  if (!active_scope) {
    allocation_fail(ARCHBIRD_CONFLICT);
    return NULL;
  }
  if (!allocation_size(size, &total) ||
      payload > active_scope->byte_limit - active_scope->bytes) {
    allocation_fail(ARCHBIRD_LIMIT_EXCEEDED);
    return NULL;
  }
  allocation = (AbTreeSitterAllocation *)ab_malloc(active_scope->engine, total);
  if (!allocation) {
    allocation_fail(ARCHBIRD_OUT_OF_MEMORY);
    return NULL;
  }
  allocation->value.magic = AB_TREE_SITTER_ALLOCATION_MAGIC;
  allocation->value.size = payload;
  allocation->value.previous = NULL;
  allocation->value.next = active_scope->head;
  if (active_scope->head)
    active_scope->head->value.previous = allocation;
  active_scope->head = allocation;
  active_scope->bytes += payload;
  return allocation + 1;
}

void *ab_tree_sitter_calloc(size_t count, size_t size) {
  size_t total;
  void *pointer;
  if (size && count > SIZE_MAX / size) {
    allocation_fail(ARCHBIRD_LIMIT_EXCEEDED);
    return NULL;
  }
  total = count * size;
  pointer = ab_tree_sitter_malloc(total);
  if (pointer)
    memset(pointer, 0, total ? total : 1);
  return pointer;
}

void *ab_tree_sitter_realloc(void *pointer, size_t size) {
  AbTreeSitterAllocation *allocation;
  AbTreeSitterAllocation *resized;
  size_t payload = size ? size : 1;
  size_t total;
  size_t previous_size;
  if (!pointer)
    return ab_tree_sitter_malloc(size);
  if (!size) {
    ab_tree_sitter_free(pointer);
    return NULL;
  }
  if (!active_scope) {
    allocation_fail(ARCHBIRD_CONFLICT);
    return NULL;
  }
  allocation = (AbTreeSitterAllocation *)pointer - 1;
  if (allocation->value.magic != AB_TREE_SITTER_ALLOCATION_MAGIC) {
    allocation_fail(ARCHBIRD_CONFLICT);
    return NULL;
  }
  previous_size = allocation->value.size;
  if (!allocation_size(size, &total) ||
      payload >
          active_scope->byte_limit - (active_scope->bytes - previous_size)) {
    allocation_fail(ARCHBIRD_LIMIT_EXCEEDED);
    return NULL;
  }
  resized = (AbTreeSitterAllocation *)ab_realloc(active_scope->engine,
                                                 allocation, total);
  if (!resized) {
    allocation_fail(ARCHBIRD_OUT_OF_MEMORY);
    return NULL;
  }
  if (resized->value.previous)
    resized->value.previous->value.next = resized;
  else
    active_scope->head = resized;
  if (resized->value.next)
    resized->value.next->value.previous = resized;
  resized->value.size = payload;
  active_scope->bytes = active_scope->bytes - previous_size + payload;
  return resized + 1;
}

void ab_tree_sitter_free(void *pointer) {
  AbTreeSitterAllocation *allocation;
  if (!pointer)
    return;
  if (!active_scope) {
    allocation_fail(ARCHBIRD_CONFLICT);
    return;
  }
  allocation = (AbTreeSitterAllocation *)pointer - 1;
  if (allocation->value.magic != AB_TREE_SITTER_ALLOCATION_MAGIC) {
    allocation_fail(ARCHBIRD_CONFLICT);
    return;
  }
  if (allocation->value.previous)
    allocation->value.previous->value.next = allocation->value.next;
  else
    active_scope->head = allocation->value.next;
  if (allocation->value.next)
    allocation->value.next->value.previous = allocation->value.previous;
  active_scope->bytes -= allocation->value.size;
  allocation->value.magic = 0;
  ab_free(active_scope->engine, allocation);
}

static void release_scope(AbTreeSitterAllocationScope *scope) {
  while (scope->head) {
    AbTreeSitterAllocation *allocation = scope->head;
    scope->head = allocation->value.next;
    allocation->value.magic = 0;
    ab_free(scope->engine, allocation);
  }
  scope->bytes = 0;
}

ArchbirdStatus ab_tree_sitter_with_allocator(ArchbirdEngine *engine,
                                             size_t byte_limit,
                                             AbTreeSitterOperation operation,
                                             void *user_data,
                                             int *out_resource_limited) {
  AbTreeSitterAllocationScope *scope;
  ArchbirdStatus status;
  int trapped;
  if (!engine || !byte_limit || !operation || !out_resource_limited)
    return ARCHBIRD_INVALID_ARGUMENT;
  configure_tree_sitter_allocator();
  *out_resource_limited = 0;
  if (active_scope)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "nested Tree-sitter allocation scope");
  scope = (AbTreeSitterAllocationScope *)ab_calloc(engine, 1, sizeof(*scope));
  if (!scope)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory creating Tree-sitter scope");
  scope->engine = engine;
  scope->byte_limit = byte_limit;
  active_scope = scope;
  if (setjmp(scope->recovery) == 0) {
    status = operation(user_data);
  } else {
    status = scope->failure;
  }
  trapped = scope->trapped;
  release_scope(scope);
  active_scope = NULL;
  ab_free(engine, scope);
  if (trapped && status == ARCHBIRD_OUT_OF_MEMORY)
    return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                              "out of memory in Tree-sitter provider");
  if (trapped && status == ARCHBIRD_LIMIT_EXCEEDED) {
    *out_resource_limited = 1;
    return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                              "Tree-sitter provider allocation limit exceeded");
  }
  if (trapped && status == ARCHBIRD_CONFLICT)
    return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                              "invalid Tree-sitter allocator state");
  return status;
}
