#ifndef ARCHBIRD_PATH_INTERNAL_H
#define ARCHBIRD_PATH_INTERNAL_H

#include "base/archbird_internal.h"
#include "base/json_value.h"
#include "base/render_internal.h"

ArchbirdStatus ab_path_execute_value(ArchbirdEngine *engine, const AbValue *map,
                                     const AbValue *resolution,
                                     const AbValue *request, AbBuffer *out);

ArchbirdStatus ab_path_render_markdown_value(ArchbirdEngine *engine,
                                             const AbValue *artifact,
                                             size_t max_chars, AbBuffer *out);

#endif
