#ifndef ARCHBIRD_PROJECTION_COMPONENT_MEMBERSHIP_H
#define ARCHBIRD_PROJECTION_COMPONENT_MEMBERSHIP_H

#include "json_value.h"

typedef struct AbVerifyMembershipAssignment {
  const AbString *path;
  size_t component_index;
} AbVerifyMembershipAssignment;

typedef struct AbVerifyMembershipFile {
  const AbValue *row;
  const AbString *path;
  size_t assignment_start;
  size_t assignment_count;
} AbVerifyMembershipFile;

typedef struct AbVerifyMembershipComponent {
  const AbValue *row;
  const AbString *name;
  size_t file_count;
  size_t exclusive_count;
  size_t overlap_count;
} AbVerifyMembershipComponent;

typedef struct AbVerifyMembershipIndex {
  AbVerifyMembershipFile *files;
  size_t file_count;
  AbVerifyMembershipComponent *components;
  size_t component_count;
  AbVerifyMembershipAssignment *assignments;
  size_t assignment_count;
  size_t assigned_count;
  size_t unassigned_count;
  size_t overlap_count;
  size_t empty_component_count;
  const char *message;
  int current;
} AbVerifyMembershipIndex;

ArchbirdStatus ab_verify_membership_index_build(ArchbirdEngine *engine,
                                                const AbValue *map,
                                                AbVerifyMembershipIndex *out);

void ab_verify_membership_index_free(ArchbirdEngine *engine,
                                     AbVerifyMembershipIndex *index);

const AbVerifyMembershipComponent *
ab_verify_membership_component(const AbVerifyMembershipIndex *index,
                               const AbString *name, size_t *out_index);

const AbVerifyMembershipFile *
ab_verify_membership_file(const AbVerifyMembershipIndex *index,
                          const AbString *path);

#endif
