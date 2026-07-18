#ifndef ARCHBIRD_MODEL_H
#define ARCHBIRD_MODEL_H

#include "archbird_internal.h"

typedef struct AbString {
  char *data;
  size_t length;
} AbString;

typedef struct AbStringArray {
  AbString *items;
  size_t count;
} AbStringArray;

typedef struct AbProducer {
  AbString name;
  AbString version;
  uint8_t implementation_sha256[32];
  uint8_t configuration_sha256[32];
  int has_configuration_sha256;
  AbString runtime;
  int has_runtime;
} AbProducer;

typedef struct AbManifestFile {
  AbString path;
  uint8_t sha256[32];
  size_t byte_length;
  AbString language;
  int has_language;
  AbString layer;
  int has_layer;
  AbStringArray roles;
} AbManifestFile;

int ab_manifest_file_has_role(const AbManifestFile *file, const char *role);

typedef struct AbDiscoveryCoverage {
  uint64_t assets;
  uint64_t ignored;
  uint64_t inventory_files;
  uint64_t oversized;
  uint64_t pruned_directories;
  uint64_t selected;
  uint64_t unsupported_known;
} AbDiscoveryCoverage;

typedef struct AbManifestResolution {
  AbString profile_name;
  uint8_t profile_implementation_sha256[32];
  uint8_t sha256[32];
  AbDiscoveryCoverage coverage;
} AbManifestResolution;

typedef struct AbSourceManifest {
  AbString project;
  AbProducer producer;
  uint8_t configuration_sha256[32];
  int has_configuration_sha256;
  AbManifestResolution resolution;
  int has_resolution;
  AbManifestFile *files;
  size_t file_count;
} AbSourceManifest;

typedef enum AbValueKind {
  AB_VALUE_NULL = 0,
  AB_VALUE_BOOL = 1,
  AB_VALUE_INTEGER = 2,
  AB_VALUE_REAL = 3,
  AB_VALUE_STRING = 4,
  AB_VALUE_ARRAY = 5,
  AB_VALUE_OBJECT = 6
} AbValueKind;

typedef struct AbValue AbValue;
typedef struct AbObjectField AbObjectField;

struct AbValue {
  AbValueKind kind;
  union {
    int boolean;
    double real;
    AbString text;
    struct {
      AbValue *items;
      size_t count;
    } array;
    struct {
      AbObjectField *fields;
      size_t count;
    } object;
  } as;
};

struct AbObjectField {
  AbString name;
  AbValue value;
};

typedef struct AbCapability {
  AbString domain;
  AbString coverage;
  AbStringArray claims;
  AbString boundary;
  int has_boundary;
} AbCapability;

typedef struct AbResolution {
  AbString state;
  AbStringArray targets;
  AbString reason;
  int has_reason;
} AbResolution;

typedef struct AbFact {
  AbString id;
  AbString domain;
  AbString kind;
  AbString claim;
  AbString project;
  AbString path;
  size_t span_start;
  size_t span_end;
  int correlate_by_span;
  AbString key;
  AbString name;
  int has_name;
  AbObjectField *attributes;
  size_t attribute_count;
  AbResolution resolution;
  int has_resolution;
} AbFact;

typedef struct AbDiagnostic {
  AbString severity;
  AbString code;
  AbString message;
  AbString project;
  int has_project;
  AbString path;
  int has_path;
  size_t span_start;
  size_t span_end;
  int has_span;
} AbDiagnostic;

typedef struct AbProviderInput {
  AbString project;
  AbString path;
  int has_path;
  uint8_t source_manifest_sha256[32];
  int has_source_manifest_sha256;
  uint8_t source_sha256[32];
  int has_source_sha256;
} AbProviderInput;

typedef struct AbSubject {
  AbString scope;
  AbString project;
  int has_project;
  AbString path;
  int has_path;
  AbString name;
  int has_name;
} AbSubject;

typedef struct AbProviderBundle {
  AbSubject subject;
  AbProducer producer;
  AbProviderInput *inputs;
  size_t input_count;
  AbCapability *capabilities;
  size_t capability_count;
  AbFact *facts;
  size_t fact_count;
  AbDiagnostic *diagnostics;
  size_t diagnostic_count;
  ArchbirdProviderMode mode;
  uint8_t sha256[32];
  char sha256_hex[65];
} AbProviderBundle;

ArchbirdStatus ab_string_copy(ArchbirdEngine *engine, AbString *out,
                              const char *data, size_t length);
void ab_string_free(ArchbirdEngine *engine, AbString *value);
int ab_string_compare(const AbString *left, const AbString *right);
int ab_string_equal(const AbString *left, const AbString *right);
int ab_value_equal(const AbValue *left, const AbValue *right);
ArchbirdStatus ab_value_copy(ArchbirdEngine *engine, AbValue *out,
                             const AbValue *source);
void ab_value_free(ArchbirdEngine *engine, AbValue *value);

void ab_fact_free(ArchbirdEngine *engine, AbFact *fact);
int ab_fact_equal(const AbFact *left, const AbFact *right);
ArchbirdStatus ab_fact_copy(ArchbirdEngine *engine, AbFact *out,
                            const AbFact *source);
ArchbirdStatus ab_fact_merge_compatible(ArchbirdEngine *engine, AbFact *target,
                                        const AbFact *source);
int ab_fact_names_compatible(const AbFact *left, const AbFact *right);
int ab_fact_attribute_is_presentation(const AbString *name);

void ab_source_manifest_free(ArchbirdEngine *engine,
                             AbSourceManifest *manifest);
void ab_provider_bundle_free(ArchbirdEngine *engine, AbProviderBundle *bundle);

#endif
