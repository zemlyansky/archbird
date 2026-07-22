#ifndef ARCHBIRD_LEXICAL_C_CONSTANTS_H
#define ARCHBIRD_LEXICAL_C_CONSTANTS_H

#include "fact_builder.h"
#include "lexical/tokenizer.h"

ArchbirdStatus ab_c_scan_constant_facts(AbBundleBuilder *builder,
                                        const AbTokenList *tokens);

#endif
