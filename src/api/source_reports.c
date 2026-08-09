#include <archbird/archbird.h>

#include "base/archbird_internal.h"
#include "base/json_value.h"
#include "base/render_internal.h"
#include "interchange/reports/source_report.h"

#include <string.h>

static int compare_path(const AbString *wanted,
                        const ArchbirdSourceView *source) {
  size_t shared = wanted->length < source->path_length ? wanted->length
                                                       : source->path_length;
  int compared = shared ? memcmp(wanted->data, source->path, shared) : 0;
  if (compared)
    return compared;
  if (wanted->length == source->path_length)
    return 0;
  return wanted->length < source->path_length ? -1 : 1;
}

static ArchbirdStatus project_source_lookup(void *user_data,
                                            const AbString *path,
                                            const uint8_t **out_bytes,
                                            size_t *out_length) {
  const ArchbirdProject *project = (const ArchbirdProject *)user_data;
  size_t low = 0;
  size_t high = archbird_project_source_count(project);
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    ArchbirdSourceView source;
    ArchbirdStatus status;
    int compared;
    memset(&source, 0, sizeof(source));
    source.struct_size = sizeof(source);
    status = archbird_project_source(project, middle, &source);
    if (status != ARCHBIRD_OK)
      return status;
    compared = compare_path(path, &source);
    if (compared < 0)
      high = middle;
    else if (compared > 0)
      low = middle + 1;
    else {
      *out_bytes = source.bytes;
      *out_length = source.byte_length;
      return ARCHBIRD_OK;
    }
  }
  return ARCHBIRD_CONFLICT;
}

ArchbirdStatus archbird_project_render_source_markdown(
    ArchbirdEngine *engine, const ArchbirdProject *project,
    const uint8_t *artifact_json, size_t artifact_length,
    ArchbirdReportDetail detail, size_t max_chars, ArchbirdWriteFn write_fn,
    void *user_data) {
  AbValue artifact = {0};
  AbBuffer report;
  ArchbirdStatus status;
  if (!engine || !project || !artifact_json || !artifact_length || !write_fn ||
      detail < ARCHBIRD_REPORT_DETAIL_COMPACT ||
      detail > ARCHBIRD_REPORT_DETAIL_FULL)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(engine);
  if (status != ARCHBIRD_OK)
    return status;
  ab_buffer_init(&report, engine);
  status =
      ab_json_value_decode(engine, artifact_json, artifact_length, &artifact);
  if (status == ARCHBIRD_OK)
    status = ab_source_report_markdown(engine, &artifact, detail, max_chars,
                                       project_source_lookup, (void *)project,
                                       &report);
  if (status == ARCHBIRD_OK &&
      write_fn(user_data, report.data, report.length) != 0)
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "source report callback failed");
  ab_buffer_free(&report);
  ab_value_free(engine, &artifact);
  return status;
}
