#ifndef ARCHBIRD_PROJECT_PROVIDER_MERGE_H
#define ARCHBIRD_PROJECT_PROVIDER_MERGE_H

#include "evidence/project/project_model.h"

ArchbirdStatus ab_provider_collect_domains(ArchbirdEngine *engine,
                                           const AbProjectProviderStore *store,
                                           AbDomainSelection **out_domains,
                                           size_t *out_count);

/* Returned facts and ledgers retain non-owning references into providers;
 * the provider store must outlive the result and remain byte-stable. */
ArchbirdStatus ab_provider_merge(ArchbirdEngine *engine,
                                 const AbProjectProviderStore *providers,
                                 AbProjectMergeResult *out_result);

void ab_project_merge_result_destroy(ArchbirdEngine *engine,
                                     AbProjectMergeResult *result);

#endif
