#ifndef ARCHBIRD_EVIDENCE_RENDER_H
#define ARCHBIRD_EVIDENCE_RENDER_H

#include "model.h"
#include "render_internal.h"

ArchbirdStatus ab_provider_bundle_render_compact(ArchbirdEngine *engine,
                                                 const AbProviderBundle *bundle,
                                                 AbBuffer *out);

ArchbirdStatus ab_provider_bundle_digest(ArchbirdEngine *engine,
                                         AbProviderBundle *bundle);

#endif
