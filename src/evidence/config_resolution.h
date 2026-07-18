#ifndef ARCHBIRD_CONFIG_RESOLUTION_H
#define ARCHBIRD_CONFIG_RESOLUTION_H

#include <archbird/archbird.h>

ArchbirdStatus
ab_discovery_resolve(ArchbirdEngine *engine, const uint8_t *config_json,
                     size_t config_length, const uint8_t *request_json,
                     size_t request_length, const uint8_t *inventory_json,
                     size_t inventory_length, uint32_t json_flags,
                     ArchbirdWriteFn write_fn, void *user_data);

#endif
