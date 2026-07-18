#ifndef ARCHBIRD_MAP_REFERENCES_H
#define ARCHBIRD_MAP_REFERENCES_H

#include "map_internal.h"

typedef struct AbMapReferenceResolution {
  const AbManifestFile *target;
  const AbFact *target_fact;
  const AbFact *callable_fact;
  const AbString *target_symbol;
  const char *relation;
  int exact;
} AbMapReferenceResolution;

/* Resolve one provider-emitted imported attribute/decorator/receiver
 * occurrence through proven module bindings, package export origins, and
 * qualified symbols.  A non-exact result is navigation evidence only and
 * must never become a call edge or a direct test route. */
ArchbirdStatus ab_map_resolve_imported_reference(AbMapState *state,
                                                 const AbManifestFile *source,
                                                 const AbFact *fact,
                                                 AbMapReferenceResolution *out);

/* Resolve a direct imported-name use through its mapped module and exact
 * symbol definition.  A module match without the named symbol remains a
 * candidate and cannot become a direct route. */
ArchbirdStatus ab_map_resolve_imported_name_reference(
    AbMapState *state, const AbManifestFile *source, const AbFact *fact,
    AbMapReferenceResolution *out);

/* Resolve a language-native provider's exact reference target.  The generic
 * reference-targets contract requires a unique resolution plus mapped
 * target_path and target_symbol attributes.  Provider-specific AST/compiler
 * objects never enter the Map core. */
ArchbirdStatus ab_map_resolve_provider_reference(AbMapState *state,
                                                 const AbFact *fact,
                                                 AbMapReferenceResolution *out);

ArchbirdStatus ab_map_add_reference_edges(AbMapState *state);

#endif
