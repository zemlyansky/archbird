#ifndef ARCHBIRD_SCIP_SCANNER_H
#define ARCHBIRD_SCIP_SCANNER_H

#include "base/model.h"
#include "evidence/config.h"

ArchbirdStatus ab_scan_scip_indexes(ArchbirdEngine *engine,
                                    ArchbirdProject *project,
                                    ArchbirdProviderMode mode);

#endif
