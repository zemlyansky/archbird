#include <archbird/archbird.h>

#include "../../base/archbird_internal.h"
#include "../../base/utf8.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define AB_DIFF_DEFAULT_CONTEXT ((size_t)3)
#define AB_DIFF_DEFAULT_MAX_WORK ((size_t)16 * 1024 * 1024)

typedef struct DiffWriter {
  ArchbirdEngine *engine;
  ArchbirdWriteFn write_fn;
  void *user_data;
} DiffWriter;

typedef struct DiffLine {
  size_t offset;
  size_t length;
} DiffLine;

typedef enum DiffOperationKind {
  DIFF_EQUAL = 0,
  DIFF_DELETE = 1,
  DIFF_INSERT = 2
} DiffOperationKind;

typedef struct DiffOperation {
  DiffOperationKind kind;
  DiffLine line;
  const uint8_t *source;
} DiffOperation;

typedef struct ExactDiff {
  DiffLine *before_lines;
  DiffLine *after_lines;
  ptrdiff_t **trace;
  ptrdiff_t *frontier;
  ptrdiff_t *next;
  DiffOperation *operations;
  size_t before_count;
  size_t after_count;
  size_t trace_count;
  size_t trace_capacity;
  size_t operation_count;
  size_t width;
} ExactDiff;

static int size_add(size_t left, size_t right, size_t *out) {
  if (right > SIZE_MAX - left)
    return 0;
  *out = left + right;
  return 1;
}

static int size_multiply(size_t left, size_t right, size_t *out) {
  if (left && right > SIZE_MAX / left)
    return 0;
  *out = left * right;
  return 1;
}

static ArchbirdStatus diff_write(DiffWriter *writer, const void *bytes,
                                 size_t length) {
  if (!length)
    return ARCHBIRD_OK;
  if (writer->write_fn(writer->user_data, (const uint8_t *)bytes, length) != 0)
    return archbird_error_set(writer->engine, ARCHBIRD_WRITE_FAILED,
                              ARCHBIRD_NO_OFFSET,
                              "unified diff output callback failed");
  return ARCHBIRD_OK;
}

static ArchbirdStatus diff_literal(DiffWriter *writer, const char *literal) {
  return diff_write(writer, literal, strlen(literal));
}

static ArchbirdStatus diff_size(DiffWriter *writer, size_t value) {
  char rendered[32];
  int length = snprintf(rendered, sizeof(rendered), "%zu", value);
  if (length < 0 || (size_t)length >= sizeof(rendered))
    return archbird_error_set(writer->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "unified diff line number is too large");
  return diff_write(writer, rendered, (size_t)length);
}

static int valid_utf8(const uint8_t *bytes, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    size_t width;
    if (bytes[offset] == 0)
      return 0;
    width = ab_utf8_scalar_length(bytes, length, offset);
    if (!width)
      return 0;
    offset += width;
  }
  return 1;
}

static ArchbirdStatus validate_path(ArchbirdEngine *engine, const char *path,
                                    size_t length, const char *name) {
  size_t index;
  size_t segment_start = 0;
  if (!path)
    return length == 0 ? ARCHBIRD_OK
                       : archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                                            ARCHBIRD_NO_OFFSET,
                                            "%s length requires a path", name);
  if (!length)
    return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                              ARCHBIRD_NO_OFFSET, "%s must not be empty", name);
  if (length > engine->options.max_string_bytes)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "%s exceeds max_string_bytes",
                              name);
  if (path[0] == '/' || (length >= 2 &&
                         ((path[0] >= 'A' && path[0] <= 'Z') ||
                          (path[0] >= 'a' && path[0] <= 'z')) &&
                         path[1] == ':'))
    return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                              ARCHBIRD_NO_OFFSET,
                              "%s must be repository-relative", name);
  if (!valid_utf8((const uint8_t *)path, length))
    return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                              ARCHBIRD_NO_OFFSET,
                              "%s must be valid non-NUL UTF-8", name);
  for (index = 0; index <= length; index++) {
    int boundary = index == length || path[index] == '/';
    if (index < length &&
        ((unsigned char)path[index] < 0x20 || path[index] == '\\'))
      return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                                ARCHBIRD_NO_OFFSET,
                                "%s is not a canonical repository path", name);
    if (!boundary)
      continue;
    if (index == segment_start ||
        (index - segment_start == 1 && path[segment_start] == '.') ||
        (index - segment_start == 2 && path[segment_start] == '.' &&
         path[segment_start + 1] == '.'))
      return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                                ARCHBIRD_NO_OFFSET,
                                "%s is not a canonical repository path", name);
    segment_start = index + 1;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus
validate_metadata(ArchbirdEngine *engine,
                  const ArchbirdUnifiedDiffOptions *options) {
  size_t index;
  if (options->metadata_length > engine->options.max_string_bytes)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "unified diff metadata exceeds max_string_bytes");
  if (!options->metadata)
    return options->metadata_length == 0
               ? ARCHBIRD_OK
               : archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                                    ARCHBIRD_NO_OFFSET,
                                    "unified diff metadata length requires "
                                    "metadata bytes");
  if (!options->metadata_length)
    return ARCHBIRD_OK;
  if (!valid_utf8(options->metadata, options->metadata_length) ||
      options->metadata[options->metadata_length - 1] != '\n')
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_ARGUMENT, ARCHBIRD_NO_OFFSET,
        "unified diff metadata must be valid newline-terminated UTF-8");
  for (index = 0; index < options->metadata_length; index++) {
    uint8_t byte = options->metadata[index];
    if (byte == '\r' || (byte < 0x20 && byte != '\n' && byte != '\t'))
      return archbird_error_set(
          engine, ARCHBIRD_INVALID_ARGUMENT, ARCHBIRD_NO_OFFSET,
          "unified diff metadata contains an invalid control byte");
  }
  return ARCHBIRD_OK;
}

void archbird_unified_diff_options_init(ArchbirdUnifiedDiffOptions *options) {
  if (!options)
    return;
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
  options->context_lines = AB_DIFF_DEFAULT_CONTEXT;
  options->max_work_bytes = AB_DIFF_DEFAULT_MAX_WORK;
}

static size_t line_count(const uint8_t *bytes, size_t length) {
  size_t count = 0;
  size_t index;
  for (index = 0; index < length; index++) {
    if (bytes[index] == '\n')
      count++;
  }
  if (length && bytes[length - 1] != '\n')
    count++;
  return count;
}

static void split_lines(const uint8_t *bytes, size_t length, DiffLine *lines) {
  size_t count = 0;
  size_t start = 0;
  size_t index;
  for (index = 0; index < length; index++) {
    if (bytes[index] == '\n') {
      lines[count].offset = start;
      lines[count].length = index - start + 1;
      count++;
      start = index + 1;
    }
  }
  if (start < length) {
    lines[count].offset = start;
    lines[count].length = length - start;
  }
}

static int lines_equal(const uint8_t *before, const DiffLine *left,
                       const uint8_t *after, const DiffLine *right) {
  return left->length == right->length &&
         (!left->length || memcmp(before + left->offset, after + right->offset,
                                  left->length) == 0);
}

static void exact_diff_free(ArchbirdEngine *engine, ExactDiff *diff) {
  size_t index;
  if (!diff)
    return;
  for (index = 0; index < diff->trace_count; index++)
    ab_free(engine, diff->trace[index]);
  ab_free(engine, diff->trace);
  ab_free(engine, diff->frontier);
  ab_free(engine, diff->next);
  ab_free(engine, diff->operations);
  ab_free(engine, diff->before_lines);
  ab_free(engine, diff->after_lines);
  memset(diff, 0, sizeof(*diff));
}

static int reserve_work(size_t *used, size_t amount, size_t limit) {
  size_t next;
  if (!size_add(*used, amount, &next) || next > limit)
    return 0;
  *used = next;
  return 1;
}

/*
 * Return 1 with an exact operation stream, or 0 to request the bounded
 * deterministic fallback. Allocation failure also falls back: rendering the
 * coarse diff itself requires no temporary heap storage.
 */
static int exact_diff_build(ArchbirdEngine *engine, const uint8_t *before,
                            size_t before_length, const uint8_t *after,
                            size_t after_length, size_t work_limit,
                            ExactDiff *diff) {
  size_t line_bytes;
  size_t pointer_bytes;
  size_t row_bytes;
  size_t operation_bytes;
  size_t used = 0;
  size_t maximum;
  size_t offset;
  size_t distance;
  size_t solved_distance = 0;
  int solved = 0;
  memset(diff, 0, sizeof(*diff));
  diff->before_count = line_count(before, before_length);
  diff->after_count = line_count(after, after_length);
  if (!size_add(diff->before_count, diff->after_count, &maximum) ||
      maximum > (size_t)PTRDIFF_MAX - 2 || maximum > (SIZE_MAX - 3) / 2)
    return 0;
  diff->width = maximum * 2 + 3;
  diff->trace_capacity = maximum + 1;
  if (!size_multiply(diff->before_count, sizeof(*diff->before_lines),
                     &line_bytes) ||
      !reserve_work(&used, line_bytes, work_limit))
    return 0;
  diff->before_lines =
      (DiffLine *)ab_malloc(engine, line_bytes ? line_bytes : 1);
  if (!diff->before_lines)
    goto fallback;
  if (!size_multiply(diff->after_count, sizeof(*diff->after_lines),
                     &line_bytes) ||
      !reserve_work(&used, line_bytes, work_limit))
    goto fallback;
  diff->after_lines =
      (DiffLine *)ab_malloc(engine, line_bytes ? line_bytes : 1);
  if (!diff->after_lines)
    goto fallback;
  split_lines(before, before_length, diff->before_lines);
  split_lines(after, after_length, diff->after_lines);
  if (!size_multiply(diff->trace_capacity, sizeof(*diff->trace),
                     &pointer_bytes) ||
      !reserve_work(&used, pointer_bytes, work_limit))
    goto fallback;
  diff->trace = (ptrdiff_t **)ab_calloc(engine, diff->trace_capacity,
                                        sizeof(*diff->trace));
  if (!diff->trace)
    goto fallback;
  if (!size_multiply(diff->width, sizeof(*diff->frontier), &row_bytes) ||
      !reserve_work(&used, row_bytes, work_limit) ||
      !reserve_work(&used, row_bytes, work_limit))
    goto fallback;
  diff->frontier = (ptrdiff_t *)ab_malloc(engine, row_bytes);
  diff->next = (ptrdiff_t *)ab_malloc(engine, row_bytes);
  if (!diff->frontier || !diff->next)
    goto fallback;
  for (distance = 0; distance < diff->width; distance++)
    diff->frontier[distance] = -1;
  offset = maximum + 1;
  diff->frontier[offset + 1] = 0;
  for (distance = 0; distance <= maximum; distance++) {
    ptrdiff_t diagonal;
    if (!reserve_work(&used, row_bytes, work_limit))
      goto fallback;
    diff->trace[distance] = (ptrdiff_t *)ab_malloc(engine, row_bytes);
    if (!diff->trace[distance])
      goto fallback;
    memcpy(diff->trace[distance], diff->frontier, row_bytes);
    diff->trace_count++;
    memcpy(diff->next, diff->frontier, row_bytes);
    for (diagonal = -(ptrdiff_t)distance; diagonal <= (ptrdiff_t)distance;
         diagonal += 2) {
      ptrdiff_t x;
      ptrdiff_t y;
      size_t slot = (size_t)((ptrdiff_t)offset + diagonal);
      if (diagonal == -(ptrdiff_t)distance ||
          (diagonal != (ptrdiff_t)distance &&
           diff->frontier[slot - 1] < diff->frontier[slot + 1]))
        x = diff->frontier[slot + 1];
      else
        x = diff->frontier[slot - 1] + 1;
      y = x - diagonal;
      while (x >= 0 && y >= 0 && (size_t)x < diff->before_count &&
             (size_t)y < diff->after_count &&
             lines_equal(before, &diff->before_lines[x], after,
                         &diff->after_lines[y])) {
        x++;
        y++;
      }
      diff->next[slot] = x;
      if ((size_t)x >= diff->before_count && (size_t)y >= diff->after_count) {
        solved = 1;
        solved_distance = distance;
        break;
      }
    }
    if (solved)
      break;
    {
      ptrdiff_t *temporary = diff->frontier;
      diff->frontier = diff->next;
      diff->next = temporary;
    }
  }
  if (!solved)
    goto fallback;
  if (!size_multiply(maximum, sizeof(*diff->operations), &operation_bytes) ||
      !reserve_work(&used, operation_bytes, work_limit))
    goto fallback;
  diff->operations =
      (DiffOperation *)ab_malloc(engine, operation_bytes ? operation_bytes : 1);
  if (!diff->operations)
    goto fallback;
  {
    ptrdiff_t x = (ptrdiff_t)diff->before_count;
    ptrdiff_t y = (ptrdiff_t)diff->after_count;
    size_t depth = solved_distance;
    while (1) {
      ptrdiff_t *previous = diff->trace[depth];
      ptrdiff_t diagonal = x - y;
      ptrdiff_t previous_diagonal;
      ptrdiff_t previous_x;
      ptrdiff_t previous_y;
      size_t slot = (size_t)((ptrdiff_t)offset + diagonal);
      if (diagonal == -(ptrdiff_t)depth ||
          (diagonal != (ptrdiff_t)depth &&
           previous[slot - 1] < previous[slot + 1]))
        previous_diagonal = diagonal + 1;
      else
        previous_diagonal = diagonal - 1;
      previous_x = previous[(size_t)((ptrdiff_t)offset + previous_diagonal)];
      previous_y = previous_x - previous_diagonal;
      while (x > previous_x && y > previous_y) {
        DiffOperation *operation = &diff->operations[diff->operation_count++];
        operation->kind = DIFF_EQUAL;
        operation->line = diff->before_lines[(size_t)x - 1];
        operation->source = before;
        x--;
        y--;
      }
      if (depth == 0)
        break;
      if (x == previous_x) {
        DiffOperation *operation = &diff->operations[diff->operation_count++];
        operation->kind = DIFF_INSERT;
        operation->line = diff->after_lines[(size_t)y - 1];
        operation->source = after;
        y--;
      } else {
        DiffOperation *operation = &diff->operations[diff->operation_count++];
        operation->kind = DIFF_DELETE;
        operation->line = diff->before_lines[(size_t)x - 1];
        operation->source = before;
        x--;
      }
      depth--;
    }
  }
  for (distance = 0; distance < diff->operation_count / 2; distance++) {
    DiffOperation temporary = diff->operations[distance];
    diff->operations[distance] =
        diff->operations[diff->operation_count - distance - 1];
    diff->operations[diff->operation_count - distance - 1] = temporary;
  }
  return 1;

fallback:
  exact_diff_free(engine, diff);
  return 0;
}

static ArchbirdStatus render_range(DiffWriter *writer, size_t start,
                                   size_t count) {
  ArchbirdStatus status;
  if (count == 0) {
    status = diff_size(writer, start);
    if (status != ARCHBIRD_OK)
      return status;
    return diff_literal(writer, ",0");
  }
  status = diff_size(writer, start + 1);
  if (status != ARCHBIRD_OK || count == 1)
    return status;
  status = diff_literal(writer, ",");
  return status == ARCHBIRD_OK ? diff_size(writer, count) : status;
}

static ArchbirdStatus render_operation(DiffWriter *writer,
                                       const DiffOperation *operation) {
  static const char prefixes[] = {' ', '-', '+'};
  ArchbirdStatus status =
      diff_write(writer, &prefixes[(int)operation->kind], 1);
  if (status == ARCHBIRD_OK)
    status = diff_write(writer, operation->source + operation->line.offset,
                        operation->line.length);
  if (status != ARCHBIRD_OK)
    return status;
  if (!operation->line.length ||
      operation->source[operation->line.offset + operation->line.length - 1] !=
          '\n') {
    status = diff_literal(writer, "\n\\ No newline at end of file\n");
  }
  return status;
}

static ArchbirdStatus render_hunk_header(DiffWriter *writer, size_t old_start,
                                         size_t old_count, size_t new_start,
                                         size_t new_count) {
  ArchbirdStatus status = diff_literal(writer, "@@ -");
  if (status == ARCHBIRD_OK)
    status = render_range(writer, old_start, old_count);
  if (status == ARCHBIRD_OK)
    status = diff_literal(writer, " +");
  if (status == ARCHBIRD_OK)
    status = render_range(writer, new_start, new_count);
  if (status == ARCHBIRD_OK)
    status = diff_literal(writer, " @@\n");
  return status;
}

static ArchbirdStatus render_exact(DiffWriter *writer, const ExactDiff *diff,
                                   size_t context) {
  size_t search = 0;
  size_t cursor = 0;
  size_t old_line = 0;
  size_t new_line = 0;
  while (search < diff->operation_count) {
    size_t changed;
    size_t start;
    size_t end;
    size_t scan;
    size_t old_start;
    size_t new_start;
    size_t old_count = 0;
    size_t new_count = 0;
    ArchbirdStatus status;
    for (changed = search; changed < diff->operation_count; changed++) {
      if (diff->operations[changed].kind != DIFF_EQUAL)
        break;
    }
    if (changed == diff->operation_count)
      break;
    start = changed > context ? changed - context : 0;
    if (context >= diff->operation_count - changed - 1)
      end = diff->operation_count;
    else
      end = changed + context + 1;
    for (scan = changed + 1; scan < diff->operation_count; scan++) {
      size_t candidate_start;
      size_t candidate_end;
      if (diff->operations[scan].kind == DIFF_EQUAL)
        continue;
      candidate_start = scan > context ? scan - context : 0;
      if (candidate_start > end)
        break;
      if (context >= diff->operation_count - scan - 1)
        candidate_end = diff->operation_count;
      else
        candidate_end = scan + context + 1;
      if (candidate_end > end)
        end = candidate_end;
    }
    while (cursor < start) {
      if (diff->operations[cursor].kind != DIFF_INSERT)
        old_line++;
      if (diff->operations[cursor].kind != DIFF_DELETE)
        new_line++;
      cursor++;
    }
    old_start = old_line;
    new_start = new_line;
    for (scan = start; scan < end; scan++) {
      if (diff->operations[scan].kind != DIFF_INSERT)
        old_count++;
      if (diff->operations[scan].kind != DIFF_DELETE)
        new_count++;
    }
    status =
        render_hunk_header(writer, old_start, old_count, new_start, new_count);
    if (status != ARCHBIRD_OK)
      return status;
    for (; cursor < end; cursor++) {
      status = render_operation(writer, &diff->operations[cursor]);
      if (status != ARCHBIRD_OK)
        return status;
      if (diff->operations[cursor].kind != DIFF_INSERT)
        old_line++;
      if (diff->operations[cursor].kind != DIFF_DELETE)
        new_line++;
    }
    search = end;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_raw_lines(DiffWriter *writer, const uint8_t *bytes,
                                       size_t length, char prefix) {
  size_t start = 0;
  size_t index;
  ArchbirdStatus status;
  for (index = 0; index < length; index++) {
    if (bytes[index] != '\n')
      continue;
    status = diff_write(writer, &prefix, 1);
    if (status == ARCHBIRD_OK)
      status = diff_write(writer, bytes + start, index - start + 1);
    if (status != ARCHBIRD_OK)
      return status;
    start = index + 1;
  }
  if (start < length) {
    status = diff_write(writer, &prefix, 1);
    if (status == ARCHBIRD_OK)
      status = diff_write(writer, bytes + start, length - start);
    if (status == ARCHBIRD_OK)
      status = diff_literal(writer, "\n\\ No newline at end of file\n");
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_fallback(DiffWriter *writer, const uint8_t *before,
                                      size_t before_length,
                                      const uint8_t *after,
                                      size_t after_length) {
  size_t before_count = line_count(before, before_length);
  size_t after_count = line_count(after, after_length);
  ArchbirdStatus status =
      render_hunk_header(writer, 0, before_count, 0, after_count);
  if (status == ARCHBIRD_OK)
    status = render_raw_lines(writer, before, before_length, '-');
  if (status == ARCHBIRD_OK)
    status = render_raw_lines(writer, after, after_length, '+');
  return status;
}

static ArchbirdStatus render_path(DiffWriter *writer, const char *prefix,
                                  const char *path, size_t path_length) {
  ArchbirdStatus status = diff_literal(writer, prefix);
  return status == ARCHBIRD_OK ? diff_write(writer, path, path_length) : status;
}

ArchbirdStatus archbird_unified_diff(
    ArchbirdEngine *engine, const uint8_t *before, size_t before_length,
    const uint8_t *after, size_t after_length, const char *before_path,
    size_t before_path_length, const char *after_path, size_t after_path_length,
    const ArchbirdUnifiedDiffOptions *provided, ArchbirdWriteFn write_fn,
    void *user_data) {
  ArchbirdUnifiedDiffOptions resolved;
  const char *git_left;
  const char *git_right;
  size_t git_left_length;
  size_t git_right_length;
  size_t work_limit;
  int binary;
  DiffWriter writer;
  ExactDiff exact;
  ArchbirdStatus status;
  if (!engine || (!before && before_length) || (!after && after_length) ||
      !write_fn)
    return ARCHBIRD_INVALID_ARGUMENT;
  archbird_unified_diff_options_init(&resolved);
  if (provided) {
    if (provided->struct_size != sizeof(*provided))
      return ARCHBIRD_INVALID_ARGUMENT;
    resolved = *provided;
  }
  if ((!before_path && !after_path) || !resolved.max_work_bytes)
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_ARGUMENT, ARCHBIRD_NO_OFFSET,
        "unified diff requires a path and a nonzero work budget");
  if (before_length > engine->options.max_file_bytes ||
      after_length > engine->options.max_file_bytes)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "unified diff input exceeds max_file_bytes");
  status =
      validate_path(engine, before_path, before_path_length, "before_path");
  if (status == ARCHBIRD_OK)
    status = validate_path(engine, after_path, after_path_length, "after_path");
  if (status == ARCHBIRD_OK)
    status = validate_metadata(engine, &resolved);
  if (status != ARCHBIRD_OK)
    return status;
  git_left = before_path ? before_path : after_path;
  git_left_length = before_path ? before_path_length : after_path_length;
  git_right = after_path ? after_path : before_path;
  git_right_length = after_path ? after_path_length : before_path_length;
  writer.engine = engine;
  writer.write_fn = write_fn;
  writer.user_data = user_data;
  status = render_path(&writer, "diff --git a/", git_left, git_left_length);
  if (status == ARCHBIRD_OK)
    status = render_path(&writer, " b/", git_right, git_right_length);
  if (status == ARCHBIRD_OK)
    status = diff_literal(&writer, "\n");
  if (status == ARCHBIRD_OK)
    status = diff_write(&writer, resolved.metadata, resolved.metadata_length);
  if (status == ARCHBIRD_OK) {
    if (before_path)
      status = render_path(&writer, "--- a/", before_path, before_path_length);
    else
      status = diff_literal(&writer, "--- /dev/null");
  }
  if (status == ARCHBIRD_OK)
    status = diff_literal(&writer, "\n");
  if (status == ARCHBIRD_OK) {
    if (after_path)
      status = render_path(&writer, "+++ b/", after_path, after_path_length);
    else
      status = diff_literal(&writer, "+++ /dev/null");
  }
  if (status == ARCHBIRD_OK)
    status = diff_literal(&writer, "\n");
  if (status != ARCHBIRD_OK ||
      (before_length == after_length &&
       (!before_length || memcmp(before, after, before_length) == 0)))
    return status;
  binary =
      !valid_utf8(before, before_length) || !valid_utf8(after, after_length);
  if (binary) {
    status = diff_literal(&writer, "Binary files ");
    if (status == ARCHBIRD_OK) {
      if (before_path)
        status = render_path(&writer, "a/", before_path, before_path_length);
      else
        status = diff_literal(&writer, "/dev/null");
    }
    if (status == ARCHBIRD_OK)
      status = diff_literal(&writer, " and ");
    if (status == ARCHBIRD_OK) {
      if (after_path)
        status = render_path(&writer, "b/", after_path, after_path_length);
      else
        status = diff_literal(&writer, "/dev/null");
    }
    if (status == ARCHBIRD_OK)
      status = diff_literal(&writer, " differ\n");
    return status;
  }
  work_limit = resolved.max_work_bytes;
  if (work_limit > engine->options.max_index_bytes)
    work_limit = engine->options.max_index_bytes;
  if (exact_diff_build(engine, before, before_length, after, after_length,
                       work_limit, &exact)) {
    status = render_exact(&writer, &exact, resolved.context_lines);
    exact_diff_free(engine, &exact);
    return status;
  }
  return render_fallback(&writer, before, before_length, after, after_length);
}
