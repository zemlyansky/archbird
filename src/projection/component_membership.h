#ifndef ARCHBIRD_PROJECTION_COMPONENT_MEMBERSHIP_H
#define ARCHBIRD_PROJECTION_COMPONENT_MEMBERSHIP_H

#include "json_value.h"

typedef struct AbProjectionMembershipAssignment {
  const AbString *path;
  size_t component_index;
} AbProjectionMembershipAssignment;

typedef struct AbProjectionMembershipFile {
  const AbValue *row;
  const AbString *path;
  size_t assignment_start;
  size_t assignment_count;
} AbProjectionMembershipFile;

typedef struct AbProjectionMembershipComponent {
  const AbValue *row;
  const AbString *name;
  size_t file_count;
  size_t exclusive_count;
  size_t overlap_count;
} AbProjectionMembershipComponent;

typedef struct AbProjectionMembershipIndex {
  AbProjectionMembershipFile *files;
  size_t file_count;
  AbProjectionMembershipComponent *components;
  size_t component_count;
  AbProjectionMembershipAssignment *assignments;
  size_t assignment_count;
  size_t assigned_count;
  size_t unassigned_count;
  size_t overlap_count;
  size_t empty_component_count;
  const char *message;
  int current;
} AbProjectionMembershipIndex;

ArchbirdStatus
ab_projection_membership_index_build(ArchbirdEngine *engine, const AbValue *map,
                                     AbProjectionMembershipIndex *out);

void ab_projection_membership_index_free(ArchbirdEngine *engine,
                                         AbProjectionMembershipIndex *index);

const AbProjectionMembershipComponent *
ab_projection_membership_component(const AbProjectionMembershipIndex *index,
                                   const AbString *name, size_t *out_index);

const AbProjectionMembershipFile *
ab_projection_membership_file(const AbProjectionMembershipIndex *index,
                              const AbString *path);

#endif
