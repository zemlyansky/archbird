#ifndef ARCHBIRD_FILE_FACTS_H
#define ARCHBIRD_FILE_FACTS_H

#include "base/render_internal.h"
#include "evidence/project_internal.h"

ArchbirdStatus ab_render_file_facts_row(AbBuffer *buffer,
                                        ArchbirdEngine *engine,
                                        const ArchbirdProject *project,
                                        const AbManifestFile *file);

/* Map keeps bounded navigation roles that are deliberately absent from the
 * source-oriented FileFacts projection. */
ArchbirdStatus ab_render_map_file_facts_row(AbBuffer *buffer,
                                            ArchbirdEngine *engine,
                                            const ArchbirdProject *project,
                                            const AbManifestFile *file);

ArchbirdStatus ab_file_symbol_count(ArchbirdEngine *engine,
                                    const ArchbirdProject *project,
                                    const AbManifestFile *file,
                                    size_t *out_count);

#endif
