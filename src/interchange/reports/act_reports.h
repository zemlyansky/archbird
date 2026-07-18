#ifndef ARCHBIRD_INTERCHANGE_ACT_REPORTS_H
#define ARCHBIRD_INTERCHANGE_ACT_REPORTS_H

#include "act_internal.h"

ArchbirdStatus
ab_act_proposal_render_markdown(AbBuffer *buffer,
                                const AbActProposalView *proposal, int full,
                                size_t max_candidates);
ArchbirdStatus
ab_act_contract_render_markdown(AbBuffer *buffer,
                                const AbActContractView *contract);
ArchbirdStatus ab_act_result_render_markdown(AbBuffer *buffer,
                                             const AbActResultData *result);
ArchbirdStatus ab_act_result_render_sarif(AbBuffer *buffer,
                                          const AbActResultData *result);
ArchbirdStatus ab_act_result_render_junit(AbBuffer *buffer,
                                          const AbActResultData *result);

#endif
