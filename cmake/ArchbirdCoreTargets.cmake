# Coarse native-core compilation targets. Source prefixes are relative to src/.
# Keep groups dependency-first and each prefix/dependency list sorted.
set(ARCHBIRD_DECLARED_CORE_GROUPS
  foundation
  model
  navigation
  verification
  interchange
  configuration
  evidence
  mapping
  planning
  api
)

set(ARCHBIRD_CORE_TREE_SITTER_GROUP
  evidence
)

set(ARCHBIRD_CORE_GROUP_foundation_SOURCE_PREFIXES
  base
)
set(ARCHBIRD_CORE_GROUP_foundation_DEPENDENCIES
)

set(ARCHBIRD_CORE_GROUP_model_SOURCE_PREFIXES
  projection
  transformation
)
set(ARCHBIRD_CORE_GROUP_model_DEPENDENCIES
  foundation
)

set(ARCHBIRD_CORE_GROUP_navigation_SOURCE_PREFIXES
  path
  query
)
set(ARCHBIRD_CORE_GROUP_navigation_DEPENDENCIES
  foundation
  model
)

set(ARCHBIRD_CORE_GROUP_verification_SOURCE_PREFIXES
  verify
)
set(ARCHBIRD_CORE_GROUP_verification_DEPENDENCIES
  foundation
  model
)

set(ARCHBIRD_CORE_GROUP_interchange_SOURCE_PREFIXES
  interchange
)
set(ARCHBIRD_CORE_GROUP_interchange_DEPENDENCIES
  foundation
  model
  verification
)

set(ARCHBIRD_CORE_GROUP_configuration_SOURCE_PREFIXES
  configuration
  constraints
)
set(ARCHBIRD_CORE_GROUP_configuration_DEPENDENCIES
  foundation
  interchange
  model
  navigation
  verification
)

set(ARCHBIRD_CORE_GROUP_evidence_SOURCE_PREFIXES
  evidence
)
set(ARCHBIRD_CORE_GROUP_evidence_DEPENDENCIES
  configuration
  foundation
)

set(ARCHBIRD_CORE_GROUP_mapping_SOURCE_PREFIXES
  map
)
set(ARCHBIRD_CORE_GROUP_mapping_DEPENDENCIES
  evidence
  foundation
)

set(ARCHBIRD_CORE_GROUP_planning_SOURCE_PREFIXES
  act
  plan
)
set(ARCHBIRD_CORE_GROUP_planning_DEPENDENCIES
  evidence
  foundation
  model
  verification
)

set(ARCHBIRD_CORE_GROUP_api_SOURCE_PREFIXES
  api
)
set(ARCHBIRD_CORE_GROUP_api_DEPENDENCIES
  configuration
  foundation
  interchange
  model
  navigation
  planning
  verification
)
