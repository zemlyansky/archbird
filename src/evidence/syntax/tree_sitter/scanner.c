#include "syntax/tree_sitter/scanner.h"

#include "sha256.h"
#include "syntax/tree_sitter/tree_sitter_allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AbTreeSitterParse {
  AbTreeSitterScan *scan;
} AbTreeSitterParse;

static ArchbirdStatus invalid_extraction_node(AbTreeSitterScan *scan,
                                              TSNode node,
                                              const char *context) {
  size_t offset = ts_node_is_null(node) ? ARCHBIRD_NO_OFFSET
                                        : (size_t)ts_node_start_byte(node);
  const char *type = ts_node_is_null(node) ? "null" : ts_node_type(node);
  return archbird_error_set(
      scan->engine, ARCHBIRD_INVALID_SCHEMA, offset,
      "Tree-sitter %s extraction received an empty or invalid %s node (%s)",
      scan->descriptor->language_name, context, type ? type : "unknown");
}

int ab_tree_sitter_node_type(TSNode node, const char *type) {
  const char *actual;
  if (ts_node_is_null(node) || !type)
    return 0;
  actual = ts_node_type(node);
  return actual && strcmp(actual, type) == 0;
}

int ab_tree_sitter_node_slice(const AbTreeSitterScan *scan, TSNode node,
                              size_t *out_start, size_t *out_end) {
  size_t start;
  size_t end;
  if (!scan || !out_start || !out_end || ts_node_is_null(node))
    return 0;
  start = ts_node_start_byte(node);
  end = ts_node_end_byte(node);
  if (end < start || end > scan->source_length)
    return 0;
  *out_start = start;
  *out_end = end;
  return 1;
}

int ab_tree_sitter_node_has_text(const AbTreeSitterScan *scan, TSNode node) {
  size_t start;
  size_t end;
  return !ts_node_is_null(node) && !ts_node_is_missing(node) &&
         ab_tree_sitter_node_slice(scan, node, &start, &end) && start < end;
}

TSNode ab_tree_sitter_child(TSNode node, const char *field) {
  if (ts_node_is_null(node) || !field)
    return (TSNode){0};
  return ts_node_child_by_field_name(node, field, (uint32_t)strlen(field));
}

ArchbirdStatus ab_tree_sitter_add_line(AbTreeSitterScan *scan, AbFact *fact,
                                       TSNode node) {
  TSPoint point;
  if (!scan || !fact || ts_node_is_null(node))
    return ARCHBIRD_INVALID_ARGUMENT;
  point = ts_node_start_point(node);
  return ab_fact_add_u64_attribute(scan->engine, fact, "line",
                                   (uint64_t)point.row + 1);
}

ArchbirdStatus
ab_tree_sitter_add_named_fact(AbTreeSitterScan *scan, const char *domain,
                              const char *kind, TSNode node, const uint8_t *key,
                              size_t key_length, AbFact **out_fact) {
  size_t start;
  size_t end;
  if (!scan || !domain || !kind || !key || !key_length || !out_fact)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!ab_tree_sitter_node_slice(scan, node, &start, &end) || start == end)
    return invalid_extraction_node(scan, node, "named fact");
  {
    ArchbirdStatus status = ab_bundle_builder_add_fact(
        scan->builder, domain, kind, "syntax-structure", start, end, key,
        key_length, scan->source + start, end - start, out_fact);
    TSNode region = ts_node_parent(node);
    if (status == ARCHBIRD_OK)
      status = ab_tree_sitter_track_fact_region(
          scan, *out_fact, ts_node_is_null(region) ? node : region);
    return status;
  }
}

ArchbirdStatus ab_tree_sitter_add_node_fact(AbTreeSitterScan *scan,
                                            const char *domain,
                                            const char *kind, TSNode node,
                                            AbFact **out_fact) {
  size_t start;
  size_t end;
  if (!ab_tree_sitter_node_slice(scan, node, &start, &end) || start == end)
    return invalid_extraction_node(scan, node, "node fact");
  {
    ArchbirdStatus status = ab_tree_sitter_add_named_fact(
        scan, domain, kind, node, scan->source + start, end - start, out_fact);
    if (status == ARCHBIRD_OK && scan->fact_region_count) {
      scan->fact_regions[scan->fact_region_count - 1].start = start;
      scan->fact_regions[scan->fact_region_count - 1].end = end;
    }
    return status;
  }
}

ArchbirdStatus ab_tree_sitter_add_quoted_fact(AbTreeSitterScan *scan,
                                              const char *domain,
                                              const char *kind, TSNode node,
                                              AbFact **out_fact) {
  size_t start;
  size_t end;
  if (!ab_tree_sitter_node_slice(scan, node, &start, &end) || end <= start)
    return invalid_extraction_node(scan, node, "quoted fact");
  if (end - start >= 2 &&
      ((scan->source[start] == '\'' && scan->source[end - 1] == '\'') ||
       (scan->source[start] == '"' && scan->source[end - 1] == '"') ||
       (scan->source[start] == '`' && scan->source[end - 1] == '`'))) {
    start++;
    end--;
  }
  if (start == end)
    return invalid_extraction_node(scan, node, "quoted fact");
  {
    ArchbirdStatus status = ab_bundle_builder_add_fact(
        scan->builder, domain, kind, "syntax-structure", start, end,
        scan->source + start, end - start, scan->source + start, end - start,
        out_fact);
    if (status == ARCHBIRD_OK)
      status = ab_tree_sitter_track_fact_region(scan, *out_fact, node);
    return status;
  }
}

static int node_type_in(TSNode node, const char *const *types,
                        size_t type_count) {
  size_t index;
  for (index = 0; index < type_count; index++)
    if (ab_tree_sitter_node_type(node, types[index]))
      return 1;
  return 0;
}

ArchbirdStatus
ab_tree_sitter_add_qualified_fact(AbTreeSitterScan *scan, const char *domain,
                                  const char *kind, TSNode owner, TSNode name,
                                  const char *const *container_types,
                                  size_t container_count, AbFact **out_fact) {
  TSNode current;
  TSNode *parts = NULL;
  size_t part_count = 0;
  size_t capacity = 0;
  size_t total = 0;
  size_t name_start;
  size_t name_end;
  size_t index;
  uint8_t *qualified = NULL;
  size_t cursor = 0;
  ArchbirdStatus status;
  if (!scan || !domain || !kind || ts_node_is_null(owner) ||
      ts_node_is_null(name) || (!container_types && container_count) ||
      !out_fact)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!ab_tree_sitter_node_slice(scan, name, &name_start, &name_end) ||
      name_start == name_end)
    return invalid_extraction_node(scan, name, "qualified name");
  current = ts_node_parent(owner);
  while (!ts_node_is_null(current)) {
    if (node_type_in(current, container_types, container_count)) {
      TSNode ancestor_name = ab_tree_sitter_child(current, "name");
      size_t start;
      size_t end;
      /* Object-member containers such as ECMAScript `pair` name their
         declaration field `key`; callers still opt into those node types. */
      if (ts_node_is_null(ancestor_name))
        ancestor_name = ab_tree_sitter_child(current, "key");
      if (ab_tree_sitter_node_slice(scan, ancestor_name, &start, &end) &&
          start != end) {
        TSNode *resized;
        if (part_count == capacity) {
          size_t next = capacity ? capacity * 2 : 8;
          if (next > scan->engine->options.max_values)
            next = scan->engine->options.max_values;
          if (next <= capacity) {
            status = archbird_error_set(
                scan->engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
                "Tree-sitter scope nesting limit exceeded");
            goto done;
          }
          resized = (TSNode *)ab_realloc(scan->engine, parts,
                                         next * sizeof(*resized));
          if (!resized) {
            status = archbird_error_set(
                scan->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                "out of memory deriving qualified syntax name");
            goto done;
          }
          parts = resized;
          capacity = next;
        }
        parts[part_count++] = ancestor_name;
        if (total > SIZE_MAX - (end - start) - 1) {
          status = ARCHBIRD_LIMIT_EXCEEDED;
          goto done;
        }
        total += end - start + 1;
      }
    }
    current = ts_node_parent(current);
  }
  if (total > SIZE_MAX - (name_end - name_start)) {
    status = ARCHBIRD_LIMIT_EXCEEDED;
    goto done;
  }
  total += name_end - name_start;
  qualified = (uint8_t *)ab_malloc(scan->engine, total);
  if (!qualified) {
    status = archbird_error_set(scan->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing qualified syntax name");
    goto done;
  }
  for (index = part_count; index > 0; index--) {
    size_t start = 0;
    size_t end = 0;
    if (!ab_tree_sitter_node_slice(scan, parts[index - 1], &start, &end) ||
        start == end) {
      status = invalid_extraction_node(scan, parts[index - 1],
                                       "qualified ancestor name");
      goto done;
    }
    memcpy(qualified + cursor, scan->source + start, end - start);
    cursor += end - start;
    qualified[cursor++] = '.';
  }
  memcpy(qualified + cursor, scan->source + name_start, name_end - name_start);
  cursor += name_end - name_start;
  status = ab_bundle_builder_add_fact(
      scan->builder, domain, kind, "syntax-structure", name_start, name_end,
      qualified, cursor, qualified, cursor, out_fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_track_fact_region(scan, *out_fact, owner);
done:
  ab_free(scan->engine, qualified);
  ab_free(scan->engine, parts);
  return status;
}

ArchbirdStatus ab_tree_sitter_add_enclosing(AbTreeSitterScan *scan,
                                            AbFact *fact,
                                            const AbTreeSitterFrame *frame) {
  if (!scan || !fact || !frame)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!frame->has_enclosing)
    return ARCHBIRD_OK;
  return ab_fact_add_string_attribute(scan->engine, fact, "enclosing",
                                      frame->enclosing,
                                      frame->enclosing_length);
}

void ab_tree_sitter_set_enclosing_fact(AbTreeSitterFrame *frame,
                                       const AbFact *fact) {
  if (!frame || !fact || !fact->has_name || !fact->name.length)
    return;
  /* Fact strings are independently owned by the bundle and remain stable when
     the fact array grows. Traversal frames may therefore cite the canonical,
     already-qualified symbol name without copying it or retaining AbFact*. */
  frame->enclosing = (const uint8_t *)fact->name.data;
  frame->enclosing_length = fact->name.length;
  frame->has_enclosing = 1;
}

ArchbirdStatus ab_tree_sitter_track_fact_region(AbTreeSitterScan *scan,
                                                const AbFact *fact,
                                                TSNode region) {
  AbTreeSitterFactRegion *resized;
  size_t start;
  size_t end;
  size_t fact_index;
  if (!scan || !fact || !ab_tree_sitter_node_slice(scan, region, &start, &end))
    return ARCHBIRD_INVALID_ARGUMENT;
  fact_index = (size_t)(fact - scan->builder->bundle.facts);
  if (fact_index >= scan->builder->bundle.fact_count)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (scan->fact_region_count == scan->fact_region_capacity) {
    size_t next =
        scan->fact_region_capacity ? scan->fact_region_capacity * 2 : 32;
    if (next < scan->fact_region_capacity ||
        next > scan->engine->options.max_values)
      next = scan->engine->options.max_values;
    if (next <= scan->fact_region_capacity)
      return archbird_error_set(scan->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "Tree-sitter fact region limit exceeded");
    resized = (AbTreeSitterFactRegion *)ab_realloc(
        scan->engine, scan->fact_regions, next * sizeof(*resized));
    if (!resized)
      return archbird_error_set(scan->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing syntax fact regions");
    scan->fact_regions = resized;
    scan->fact_region_capacity = next;
  }
  scan->fact_regions[scan->fact_region_count].fact_index = fact_index;
  scan->fact_regions[scan->fact_region_count].start = start;
  scan->fact_regions[scan->fact_region_count].end = end;
  scan->fact_region_count++;
  return ARCHBIRD_OK;
}

static int file_role(const AbManifestFile *file, const char *role) {
  size_t index;
  size_t length = strlen(role);
  for (index = 0; index < file->roles.count; index++)
    if (file->roles.items[index].length == length &&
        memcmp(file->roles.items[index].data, role, length) == 0)
      return 1;
  return 0;
}

static ArchbirdStatus grow_frames(AbTreeSitterScan *scan,
                                  AbTreeSitterFrame **frames, size_t *capacity,
                                  size_t required) {
  AbTreeSitterFrame *resized;
  size_t next = *capacity ? *capacity : 256;
  while (next < required) {
    if (next > SIZE_MAX / 2)
      return archbird_error_set(scan->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "Tree-sitter traversal is too large");
    next *= 2;
  }
  if (next > scan->engine->options.max_values)
    next = scan->engine->options.max_values;
  if (next < required)
    return archbird_error_set(scan->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "Tree-sitter node limit exceeded");
  resized = (AbTreeSitterFrame *)ab_realloc(scan->engine, *frames,
                                            next * sizeof(*resized));
  if (!resized)
    return archbird_error_set(scan->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory traversing Tree-sitter CST");
  *frames = resized;
  *capacity = next;
  return ARCHBIRD_OK;
}

static ArchbirdStatus record_recovery(AbTreeSitterScan *scan, TSNode node,
                                      int missing) {
  size_t start;
  size_t end;
  AbTreeSitterRecovery *resized;
  if (!ab_tree_sitter_node_slice(scan, node, &start, &end))
    return ARCHBIRD_OK;
  if (scan->recovery_count == scan->recovery_capacity) {
    size_t next = scan->recovery_capacity ? scan->recovery_capacity * 2 : 16;
    if (next < scan->recovery_capacity ||
        next > scan->engine->options.max_values)
      next = scan->engine->options.max_values;
    if (next <= scan->recovery_capacity)
      return archbird_error_set(scan->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "Tree-sitter recovery region limit exceeded");
    resized = (AbTreeSitterRecovery *)ab_realloc(scan->engine, scan->recoveries,
                                                 next * sizeof(*resized));
    if (!resized)
      return archbird_error_set(
          scan->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "out of memory storing syntax recovery regions");
    scan->recoveries = resized;
    scan->recovery_capacity = next;
  }
  scan->recoveries[scan->recovery_count].start = start;
  scan->recoveries[scan->recovery_count].end = end;
  scan->recovery_count++;
  if (missing) {
    if (!scan->missing_count) {
      scan->first_missing_start = start;
      scan->first_missing_end = end;
    }
    scan->missing_count++;
  } else {
    if (!scan->error_count) {
      scan->first_error_start = start;
      scan->first_error_end = end;
    }
    scan->error_count++;
  }
  return ARCHBIRD_OK;
}

static int spans_intersect(size_t fact_start, size_t fact_end,
                           size_t recovery_start, size_t recovery_end) {
  if (recovery_start == recovery_end)
    return fact_start <= recovery_start && recovery_start <= fact_end;
  return fact_start < recovery_end && recovery_start < fact_end;
}

static ArchbirdStatus mark_recovered_facts(AbTreeSitterScan *scan) {
  size_t fact_index;
  size_t region_index = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!scan->recovery_count)
    return ARCHBIRD_OK;
  for (fact_index = 0;
       status == ARCHBIRD_OK && fact_index < scan->builder->bundle.fact_count;
       fact_index++) {
    AbFact *fact = &scan->builder->bundle.facts[fact_index];
    size_t recovery_index;
    size_t fact_start = fact->span_start;
    size_t fact_end = fact->span_end;
    size_t intersections = 0;
    if (fact->span_start == fact->span_end)
      continue;
    while (region_index < scan->fact_region_count &&
           scan->fact_regions[region_index].fact_index < fact_index)
      region_index++;
    if (region_index < scan->fact_region_count &&
        scan->fact_regions[region_index].fact_index == fact_index) {
      fact_start = scan->fact_regions[region_index].start;
      fact_end = scan->fact_regions[region_index].end;
    }
    for (recovery_index = 0; recovery_index < scan->recovery_count;
         recovery_index++) {
      const AbTreeSitterRecovery *recovery = &scan->recoveries[recovery_index];
      if (spans_intersect(fact_start, fact_end, recovery->start, recovery->end))
        intersections++;
    }
    if (!intersections)
      continue;
    status = ab_fact_add_u64_attribute(scan->engine, fact, "recovery_nodes",
                                       intersections);
    if (status == ARCHBIRD_OK)
      status = ab_fact_add_string_attribute(
          scan->engine, fact, "syntax_recovery", (const uint8_t *)"intersects",
          strlen("intersects"));
  }
  return status;
}

static ArchbirdStatus parse_and_extract(void *user_data) {
  AbTreeSitterParse *parse = (AbTreeSitterParse *)user_data;
  AbTreeSitterScan *scan = parse->scan;
  TSParser *parser = NULL;
  TSTree *tree = NULL;
  AbTreeSitterFrame *frames = NULL;
  size_t frame_count = 0;
  size_t frame_capacity = 0;
  size_t visited = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  TSNode root;
  parser = ts_parser_new();
  if (!parser ||
      !ts_parser_set_language(parser, scan->descriptor->language())) {
    status =
        archbird_error_set(scan->engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                           "Tree-sitter %s grammar is incompatible",
                           scan->descriptor->language_name);
    goto done;
  }
  tree = ts_parser_parse_string(parser, NULL, (const char *)scan->source,
                                (uint32_t)scan->source_length);
  if (!tree) {
    status =
        archbird_error_set(scan->engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                           "Tree-sitter %s parse returned no tree",
                           scan->descriptor->language_name);
    goto done;
  }
  root = ts_tree_root_node(tree);
  status = grow_frames(scan, &frames, &frame_capacity, 1);
  if (status != ARCHBIRD_OK)
    goto done;
  frames[frame_count++] = (AbTreeSitterFrame){.node = root};
  while (frame_count && status == ARCHBIRD_OK) {
    AbTreeSitterFrame frame = frames[--frame_count];
    AbTreeSitterFrame child_frame = frame;
    uint32_t child_count;
    uint32_t child_index;
    visited++;
    if (visited > scan->engine->options.max_values) {
      status = archbird_error_set(scan->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "Tree-sitter node limit exceeded");
      break;
    }
    if (ts_node_is_error(frame.node))
      status = record_recovery(scan, frame.node, 0);
    if (status == ARCHBIRD_OK && ts_node_is_missing(frame.node))
      status = record_recovery(scan, frame.node, 1);
    if (status != ARCHBIRD_OK)
      break;
    status = scan->descriptor->visit(scan, &frame, &child_frame);
    if (status != ARCHBIRD_OK)
      break;
    child_count = ts_node_child_count(frame.node);
    if (child_count > SIZE_MAX - frame_count) {
      status = archbird_error_set(scan->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "Tree-sitter child count overflow");
      break;
    }
    if (frame_count + child_count > frame_capacity) {
      status = grow_frames(scan, &frames, &frame_capacity,
                           frame_count + child_count);
      if (status != ARCHBIRD_OK)
        break;
    }
    for (child_index = child_count; child_index > 0; child_index--) {
      TSNode child = ts_node_child(frame.node, child_index - 1);
      if (!ts_node_is_named(child) && !ts_node_is_missing(child) &&
          !ts_node_is_error(child) && ts_node_child_count(child) == 0) {
        visited++;
        if (visited > scan->engine->options.max_values) {
          status = archbird_error_set(scan->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                      ARCHBIRD_NO_OFFSET,
                                      "Tree-sitter node limit exceeded");
          break;
        }
        continue;
      }
      child_frame.node = child;
      frames[frame_count++] = child_frame;
    }
  }
done:
  ab_free(scan->engine, frames);
  if (tree)
    ts_tree_delete(tree);
  if (parser)
    ts_parser_delete(parser);
  return status;
}

static ArchbirdStatus add_recovery_evidence(AbTreeSitterScan *scan) {
  AbFact *summary;
  ArchbirdStatus status;
  char message[192];
  int length;
  status = ab_bundle_builder_add_fact(
      scan->builder, "syntax-summaries", "summary", "syntax-structure", 0, 0,
      (const uint8_t *)scan->descriptor->provider_name,
      strlen(scan->descriptor->provider_name),
      (const uint8_t *)scan->descriptor->provider_name,
      strlen(scan->descriptor->provider_name), &summary);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(scan->engine, summary, "error_nodes",
                                       scan->error_count);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(scan->engine, summary, "missing_nodes",
                                       scan->missing_count);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(
        scan->engine, summary, "language",
        (const uint8_t *)scan->descriptor->language_name,
        strlen(scan->descriptor->language_name));
  if (status != ARCHBIRD_OK || file_role(scan->file, "vendor") ||
      file_role(scan->file, "generated"))
    return status;
  if (scan->error_count) {
    length = snprintf(message, sizeof(message),
                      "Tree-sitter %s recovered from %zu ERROR node(s)",
                      scan->descriptor->language_name, scan->error_count);
    if (length < 0 || (size_t)length >= sizeof(message))
      return ARCHBIRD_LIMIT_EXCEEDED;
    status = ab_bundle_builder_add_diagnostic(
        scan->builder, "warning", "tree-sitter-error", message,
        scan->first_error_start, scan->first_error_end, 1);
  }
  if (status == ARCHBIRD_OK && scan->missing_count) {
    length = snprintf(message, sizeof(message),
                      "Tree-sitter %s inserted %zu MISSING node(s)",
                      scan->descriptor->language_name, scan->missing_count);
    if (length < 0 || (size_t)length >= sizeof(message))
      return ARCHBIRD_LIMIT_EXCEEDED;
    status = ab_bundle_builder_add_diagnostic(
        scan->builder, "warning", "tree-sitter-missing", message,
        scan->first_missing_start, scan->first_missing_end, 1);
  }
  return status;
}

static ArchbirdStatus finish_resource_limited_bundle(
    AbBundleBuilder *builder, ArchbirdEngine *engine,
    const AbSourceManifest *manifest, const AbManifestFile *file,
    const uint8_t implementation_sha256[32],
    const uint8_t configuration_sha256[32],
    const AbTreeSitterDescriptor *descriptor, AbProviderBundle *out_bundle) {
  char message[192];
  size_t index;
  int length;
  ArchbirdStatus status;
  ab_bundle_builder_abort(builder);
  archbird_error_clear(engine);
  status = ab_bundle_builder_init_file(
      builder, engine, manifest, file, descriptor->provider_name,
      descriptor->provider_version, implementation_sha256,
      configuration_sha256);
  if (status == ARCHBIRD_OK)
    status =
        ab_bundle_builder_set_runtime(builder, descriptor->runtime_identity);
  for (index = 0; status == ARCHBIRD_OK && index < descriptor->capability_count;
       index++) {
    const AbTreeSitterCapabilitySpec *capability =
        &descriptor->capabilities[index];
    status = ab_bundle_builder_add_capability(
        builder, capability->domain, "none", "syntax-structure",
        "syntax evidence unavailable because a configured resource limit was "
        "reached");
  }
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_capability(
        builder, "syntax-summaries", "none", "syntax-structure",
        "parser recovery evidence unavailable because a configured resource "
        "limit was reached");
  length =
      snprintf(message, sizeof(message),
               "Tree-sitter %s analysis exceeded the configured %zu-byte "
               "syntax resource limit",
               descriptor->language_name, engine->options.max_syntax_bytes);
  if (status == ARCHBIRD_OK &&
      (length < 0 || (size_t)length >= sizeof(message)))
    status = ARCHBIRD_LIMIT_EXCEEDED;
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_diagnostic(
        builder, "warning", "tree-sitter-resource-limit", message, 0, 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_finish(builder, out_bundle);
  return status;
}

static ArchbirdStatus
finish_role_excluded_bundle(AbBundleBuilder *builder,
                            const AbTreeSitterDescriptor *descriptor,
                            AbProviderBundle *out_bundle) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < descriptor->capability_count;
       index++) {
    const AbTreeSitterCapabilitySpec *capability =
        &descriptor->capabilities[index];
    status = ab_bundle_builder_add_capability(
        builder, capability->domain, "none", "syntax-structure",
        "portable syntax analysis excludes files asserted as vendor or "
        "generated evidence");
  }
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_capability(
        builder, "syntax-summaries", "none", "syntax-structure",
        "parser recovery evidence is not produced for files asserted as "
        "vendor or generated evidence");
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_finish(builder, out_bundle);
  return status;
}

static ArchbirdStatus finish_inapplicable_bundle(AbTreeSitterScan *scan,
                                                 AbProviderBundle *out_bundle) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0;
       status == ARCHBIRD_OK && index < scan->descriptor->capability_count;
       index++) {
    const AbTreeSitterCapabilitySpec *capability =
        &scan->descriptor->capabilities[index];
    status = ab_bundle_builder_add_capability(scan->builder, capability->domain,
                                              "none", "syntax-structure",
                                              scan->inapplicable_message);
  }
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_capability(scan->builder, "syntax-summaries",
                                              "none", "syntax-structure",
                                              scan->inapplicable_message);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_diagnostic(
        scan->builder, "note", scan->inapplicable_code,
        scan->inapplicable_message, scan->inapplicable_start,
        scan->inapplicable_end, scan->has_inapplicable_span);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_finish(scan->builder, out_bundle);
  return status;
}

ArchbirdStatus ab_tree_sitter_scan_file(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbManifestFile *file, const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32],
    const uint8_t implementation_sha256[32],
    const AbTreeSitterDescriptor *descriptor, AbProviderBundle *out_bundle) {
  AbBundleBuilder builder;
  AbTreeSitterScan scan;
  AbTreeSitterParse parse;
  uint8_t configuration_sha256[32];
  size_t index;
  int resource_limited = 0;
  ArchbirdStatus status;
  if (!engine || !manifest || !file || (!source && source_length) ||
      !source_manifest_sha256 || !implementation_sha256 || !descriptor ||
      !descriptor->provider_name || !descriptor->provider_version ||
      !descriptor->configuration_identity || !descriptor->runtime_identity ||
      !descriptor->language_name || !descriptor->language ||
      !descriptor->visit || !out_bundle)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (source_length > UINT32_MAX)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "Tree-sitter source exceeds UINT32_MAX bytes");
  memset(&builder, 0, sizeof(builder));
  memset(&scan, 0, sizeof(scan));
  memset(out_bundle, 0, sizeof(*out_bundle));
  status = archbird_sha256((const uint8_t *)descriptor->configuration_identity,
                           strlen(descriptor->configuration_identity),
                           configuration_sha256);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_bundle_builder_init_file(
      &builder, engine, manifest, file, descriptor->provider_name,
      descriptor->provider_version, implementation_sha256,
      configuration_sha256);
  if (status == ARCHBIRD_OK)
    status =
        ab_bundle_builder_set_runtime(&builder, descriptor->runtime_identity);
  if (status != ARCHBIRD_OK)
    goto done;
  if (file_role(file, "vendor") || file_role(file, "generated")) {
    status = finish_role_excluded_bundle(&builder, descriptor, out_bundle);
    goto done;
  }
  scan.engine = engine;
  scan.builder = &builder;
  scan.descriptor = descriptor;
  scan.file = file;
  scan.source = source;
  scan.source_length = source_length;
  parse.scan = &scan;
  if (descriptor->prepare) {
    status = descriptor->prepare(&scan);
    if (status != ARCHBIRD_OK)
      goto done;
  }
  if (scan.inapplicable_code) {
    status = finish_inapplicable_bundle(&scan, out_bundle);
    goto done;
  }
  status = ab_tree_sitter_with_allocator(
      engine, engine->options.max_syntax_bytes, parse_and_extract, &parse,
      &resource_limited);
  if (status == ARCHBIRD_LIMIT_EXCEEDED && resource_limited)
    status = finish_resource_limited_bundle(
        &builder, engine, manifest, file, implementation_sha256,
        configuration_sha256, descriptor, out_bundle);
  if (status == ARCHBIRD_OK && out_bundle->producer.name.data)
    goto done;
  if (status == ARCHBIRD_OK)
    status = mark_recovered_facts(&scan);
  if (status == ARCHBIRD_OK)
    status = add_recovery_evidence(&scan);
  for (index = 0; status == ARCHBIRD_OK && index < descriptor->capability_count;
       index++) {
    const AbTreeSitterCapabilitySpec *capability =
        &descriptor->capabilities[index];
    const char *coverage =
        scan.error_count || scan.missing_count ? "partial" : "bounded";
    status = ab_bundle_builder_add_capability(&builder, capability->domain,
                                              coverage, "syntax-structure",
                                              capability->boundary);
  }
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_capability(
        &builder, "syntax-summaries", "complete", "syntax-structure",
        "parser recovery counts for this exact source and grammar identity");
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_finish(&builder, out_bundle);
done:
  if (descriptor && descriptor->cleanup)
    descriptor->cleanup(&scan);
  ab_free(engine, scan.recoveries);
  ab_free(engine, scan.fact_regions);
  ab_bundle_builder_abort(&builder);
  return status;
}
