#ifndef ARCHBIRD_BASE_DATE_H
#define ARCHBIRD_BASE_DATE_H

#include "model.h"

int ab_iso_date_valid(const AbString *value);
int ab_iso_date_compare(const AbString *left, const AbString *right);

#endif
