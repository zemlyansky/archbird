#ifndef ARCHBIRD_SCIP_SCANNER_H
#define ARCHBIRD_SCIP_SCANNER_H

#include "config.h"
#include "model.h"

ArchbirdStatus ab_scan_scip_indexes(ArchbirdEngine *engine,
                                    ArchbirdProject *project,
                                    ArchbirdProviderMode mode);

#endif
