#include "act/act_internal.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL %s\n", message);
    failed = 1;
  }
}

static ArchbirdStatus add_item(ArchbirdEngine *engine, AbProjectionData *fact,
                               const char *key, const char *path,
                               uint64_t line) {
  AbString key_string = {(char *)key, strlen(key)};
  AbString path_string = {(char *)path, strlen(path)};
  AbValue value = {.kind = AB_VALUE_NULL};
  AbProjectionEvidence evidence = {0};
  AbProjectionItem item = {0};
  ArchbirdStatus status =
      ab_projection_item_init(engine, &item, &key_string, &key_string, &value);
  if (status == ARCHBIRD_OK)
    status = ab_projection_evidence_init(engine, &evidence, "derived",
                                         &fact->project, &path_string, line, "",
                                         key, strlen(key));
  if (status == ARCHBIRD_OK)
    status = ab_projection_item_add_evidence(engine, &item, &evidence);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_add_item(engine, fact, &item);
  ab_projection_evidence_free(engine, &evidence);
  if (status != ARCHBIRD_OK)
    ab_projection_item_free(engine, &item);
  return status;
}

static ArchbirdStatus make_source(ArchbirdEngine *engine,
                                  AbProjectionData *source,
                                  uint64_t unsupported) {
  AbString name = {(char *)"source", 6};
  AbString project = {(char *)"fixture", 7};
  ArchbirdStatus status = ab_projection_data_init(engine, source, &name, "set",
                                                  "derived", &project);
  if (status == ARCHBIRD_OK)
    status = add_item(engine, source, "alpha", "src/alpha.c", 10);
  if (status == ARCHBIRD_OK)
    status = add_item(engine, source, "beta", "src/beta.c", 20);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(engine, source, "symbol", 4,
                                                   2, 2, unsupported, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(engine, source);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(engine, source);
  return status;
}

static ArchbirdStatus decode_value(ArchbirdEngine *engine, const char *json,
                                   AbValue *out) {
  return ab_json_value_decode(engine, (const uint8_t *)json, strlen(json), out);
}

int main(void) {
  ArchbirdEngine *engine = NULL;
  AbProjectionData source = {0};
  AbProjectionData projected = {0};
  AbProjectionData reprojected = {0};
  AbProjectionData decoded = {0};
  AbProjectionData incomplete = {0};
  AbString name = {(char *)"target", 6};
  AbValue aliases = {0};
  AbValue keys = {0};
  AbValue envelope = {0};
  AbBuffer rendered;
  ArchbirdStatus status = archbird_engine_create(NULL, &engine);
  ab_buffer_init(&rendered, engine);
  if (status == ARCHBIRD_OK)
    status = make_source(engine, &source, 0);
  check(status == ARCHBIRD_OK, "complete source construction");
  check(!strcmp(ab_projection_data_classification(&source), "complete"),
        "complete source classification");

  if (status == ARCHBIRD_OK)
    status = decode_value(engine, "{\"beta\":\"renamed\"}", &aliases);
  if (status == ARCHBIRD_OK)
    status = ab_act_project_fact(engine, &source, &name, &aliases, "all", NULL,
                                 &projected);
  check(status == ARCHBIRD_OK, "aliased projection");
  check(!strcmp(ab_projection_data_classification(&projected), "complete"),
        "all projection preserves completeness");
  check(projected.selection.universe == 4 &&
            projected.selection.selected == 2 &&
            projected.selection.excluded == 2,
        "all projection preserves selection counts");
  check(projected.item_count == 2 &&
            !strcmp(projected.items[1].key.data, "renamed") &&
            projected.items[1].evidence_count == 1 &&
            !strcmp(projected.items[1].evidence[0].path.data, "src/beta.c"),
        "alias projection preserves witnesses");
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_render(&rendered, &projected, 1);
  if (status == ARCHBIRD_OK)
    status =
        ab_json_value_decode(engine, rendered.data, rendered.length, &envelope);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_decode_artifact(engine, &envelope, &decoded);
  check(status == ARCHBIRD_OK, "projection artifact round trip");
  check(!strcmp(ab_projection_data_classification(&decoded), "complete") &&
            decoded.selection.universe == 4 &&
            !memcmp(decoded.sha256, projected.sha256, 65),
        "artifact round trip preserves completeness identity");
  ab_projection_data_free(engine, &decoded);
  {
    AbValue *completeness =
        (AbValue *)ab_value_member(&envelope, "completeness");
    AbValue *counts = completeness
                          ? (AbValue *)ab_value_member(completeness, "counts")
                          : NULL;
    AbValue *evaluated =
        counts ? (AbValue *)ab_value_member(counts, "evaluated") : NULL;
    AbValue *unknown =
        counts ? (AbValue *)ab_value_member(counts, "unknown") : NULL;
    ArchbirdStatus mutation_status = ARCHBIRD_INVALID_SCHEMA;
    if (evaluated && unknown && evaluated->kind == AB_VALUE_INTEGER &&
        unknown->kind == AB_VALUE_INTEGER && evaluated->as.text.length == 1 &&
        unknown->as.text.length == 1) {
      evaluated->as.text.data[0] = '1';
      unknown->as.text.data[0] = '1';
      mutation_status =
          ab_projection_data_decode_artifact(engine, &envelope, &decoded);
    }
    check(mutation_status == ARCHBIRD_INVALID_SCHEMA,
          "artifact rejects completeness counts inconsistent with items");
    ab_projection_data_free(engine, &decoded);
  }
  ab_value_free(engine, &envelope);
  ab_buffer_free(&rendered);
  ab_buffer_init(&rendered, engine);
  ab_projection_data_free(engine, &projected);

  if (status == ARCHBIRD_OK)
    status = decode_value(engine, "[\"renamed\"]", &keys);
  if (status == ARCHBIRD_OK)
    status = ab_act_project_fact(engine, &source, &name, &aliases, "include",
                                 &keys, &projected);
  check(status == ARCHBIRD_OK, "included projection");
  check(!strcmp(ab_projection_data_classification(&projected), "complete") &&
            projected.selection.universe == 4 &&
            projected.selection.selected == 1 &&
            projected.selection.excluded == 3 && projected.item_count == 1 &&
            !strcmp(projected.items[0].key.data, "renamed"),
        "complete selection recomputes its ledger");
  ab_projection_data_free(engine, &projected);
  ab_value_free(engine, &keys);
  ab_value_free(engine, &aliases);

  if (status == ARCHBIRD_OK)
    status = decode_value(engine, "{\"alpha\":\"same\",\"beta\":\"same\"}",
                          &aliases);
  if (status == ARCHBIRD_OK)
    status = ab_act_project_fact(engine, &source, &name, &aliases, "all", NULL,
                                 &projected);
  check(status == ARCHBIRD_OK, "colliding alias projection");
  check(!strcmp(ab_projection_data_classification(&projected), "incomplete") &&
            projected.item_count == 1 &&
            projected.items[0].evidence_count == 2 &&
            projected.selection.unknown == 2 &&
            !strcmp(projected.message.data, "normalization collision"),
        "alias collision remains incomplete with both witnesses");
  ab_value_free(engine, &aliases);
  if (status == ARCHBIRD_OK)
    status = decode_value(engine, "{}", &aliases);
  if (status == ARCHBIRD_OK)
    status = ab_act_project_fact(engine, &projected, &name, &aliases, "all",
                                 NULL, &reprojected);
  check(status == ARCHBIRD_OK &&
            !strcmp(reprojected.message.data, "normalization collision"),
        "projection preserves the source fact diagnostic");
  ab_projection_data_free(engine, &reprojected);
  ab_projection_data_free(engine, &projected);
  ab_value_free(engine, &aliases);

  if (status == ARCHBIRD_OK)
    status = make_source(engine, &incomplete, 1);
  check(
      status == ARCHBIRD_OK &&
          !strcmp(ab_projection_data_classification(&incomplete), "incomplete"),
      "incomplete source construction");
  if (status == ARCHBIRD_OK)
    status = decode_value(engine, "{}", &aliases);
  if (status == ARCHBIRD_OK)
    status = ab_act_project_fact(engine, &incomplete, &name, &aliases, "all",
                                 NULL, &projected);
  check(status == ARCHBIRD_OK &&
            strcmp(ab_projection_data_classification(&projected), "complete"),
        "all projection cannot promote incomplete evidence");
  ab_projection_data_free(engine, &projected);
  if (status == ARCHBIRD_OK)
    status = decode_value(engine, "[\"alpha\"]", &keys);
  if (status == ARCHBIRD_OK)
    status = ab_act_project_fact(engine, &incomplete, &name, &aliases,
                                 "include", &keys, &projected);
  check(status == ARCHBIRD_OK &&
            strcmp(ab_projection_data_classification(&projected), "complete"),
        "filtered projection cannot promote incomplete evidence");

  ab_projection_data_free(engine, &projected);
  ab_value_free(engine, &keys);
  ab_value_free(engine, &aliases);
  ab_projection_data_free(engine, &incomplete);
  ab_projection_data_free(engine, &source);
  ab_buffer_free(&rendered);
  archbird_engine_destroy(engine);
  return failed || status != ARCHBIRD_OK ? 1 : 0;
}
