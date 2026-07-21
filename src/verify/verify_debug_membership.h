#ifndef ARCHBIRD_VERIFY_DEBUG_MEMBERSHIP_H
#define ARCHBIRD_VERIFY_DEBUG_MEMBERSHIP_H

#include "verify_runtime.h"

int ab_verify_debug_is_membership_view(const AbString *view);

ArchbirdStatus ab_verify_debug_membership_validate(
    AbVerificationContext *context, const AbString *view,
    const AbString *project, const AbString *component);

ArchbirdStatus ab_verify_debug_membership_render_json(
    AbBuffer *buffer, const AbVerificationContext *context,
    const AbString *view, const AbString *project, const AbString *component,
    size_t limit);

ArchbirdStatus ab_verify_debug_membership_render_markdown(
    AbBuffer *buffer, const AbVerificationContext *context,
    const AbString *view, const AbString *project, const AbString *component,
    size_t limit);

#endif
