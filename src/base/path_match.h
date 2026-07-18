#ifndef ARCHBIRD_PATH_MATCH_H
#define ARCHBIRD_PATH_MATCH_H

#include "model.h"

int ab_map_path_match(const AbString *path, const AbString *pattern);
int ab_map_collection_match(const AbString *path, const AbString *pattern);
int ab_map_glob_match(const AbString *pattern, const AbString *value);

#endif
