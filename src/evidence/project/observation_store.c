#include "evidence/project/observation_store.h"

#include "evidence/test_observations.h"

#include <string.h>

ArchbirdStatus ab_project_observation_store_add(
    ArchbirdEngine *engine, ArchbirdProject *project,
    AbProjectObservationStore *store, const uint8_t *observations_json,
    size_t observations_length) {
  AbValue document = {0};
  AbValue *resized;
  ArchbirdStatus status = ab_decode_test_symbol_observations(
      engine, project, observations_json, observations_length, &document);
  if (status != ARCHBIRD_OK)
    return status;
  if (store->count == store->capacity) {
    size_t next = store->capacity ? store->capacity * 2 : 4;
    if (next > engine->options.max_values)
      next = engine->options.max_values;
    if (next <= store->capacity) {
      ab_value_free(engine, &document);
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "test observation artifact limit exceeded");
    }
    resized = (AbValue *)ab_realloc(engine, store->documents,
                                    next * sizeof(*resized));
    if (!resized) {
      ab_value_free(engine, &document);
      return archbird_error_set(
          engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "out of memory storing test-symbol observations");
    }
    store->documents = resized;
    store->capacity = next;
  }
  store->documents[store->count++] = document;
  return ARCHBIRD_OK;
}

void ab_project_observation_store_destroy(ArchbirdEngine *engine,
                                          AbProjectObservationStore *store) {
  size_t index;
  if (!store)
    return;
  for (index = 0; index < store->count; index++)
    ab_value_free(engine, &store->documents[index]);
  ab_free(engine, store->documents);
  memset(store, 0, sizeof(*store));
}
