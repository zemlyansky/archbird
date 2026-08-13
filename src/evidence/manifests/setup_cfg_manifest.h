#ifndef ARCHBIRD_SETUP_CFG_MANIFEST_H
#define ARCHBIRD_SETUP_CFG_MANIFEST_H

#include "evidence/manifests/python_package_metadata.h"

/* Extract literal setuptools setup.cfg identity and conservative package
 * layout hints. This is a bounded declarative reader, not ConfigParser or a
 * setuptools directive evaluator. attr:, file:, interpolation, conflicting
 * roots, and ambiguous package families remain unsupported. */
ArchbirdStatus ab_setup_cfg_metadata(ArchbirdEngine *engine,
                                     const uint8_t *text, size_t length,
                                     AbPythonPackageMetadata *out);

#endif
