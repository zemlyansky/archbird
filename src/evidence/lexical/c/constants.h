#ifndef ARCHBIRD_LEXICAL_C_CONSTANTS_H
#define ARCHBIRD_LEXICAL_C_CONSTANTS_H

#include "evidence/fact_builder.h"
#include "evidence/lexical/tokenizer.h"

ArchbirdStatus ab_c_scan_constant_facts(AbBundleBuilder *builder,
                                        const AbTokenList *tokens);

#endif
