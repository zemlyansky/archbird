"use strict";

const VIEW_PRESETS = Object.freeze({
  overview: Object.freeze({
    groupBy: "directory",
    level: "file",
    overlays: Object.freeze(["diagnostics", "evidence-quality"]),
    relations: Object.freeze(["builds", "bridges", "imports", "packages", "tests"]),
  }),
  architecture: Object.freeze({
    groupBy: "directory",
    level: "file",
    overlays: Object.freeze(["diagnostics", "evidence-quality"]),
    relations: Object.freeze([
      "bridges",
      "calls",
      "declarations",
      "imports",
      "packages",
      "references",
    ]),
  }),
  tests: Object.freeze({
    groupBy: "directory",
    level: "file",
    overlays: Object.freeze(["diagnostics", "evidence-quality"]),
    relations: Object.freeze(["tests"]),
  }),
  evidence: Object.freeze({
    groupBy: "directory",
    level: "file",
    overlays: Object.freeze(["diagnostics", "evidence-quality"]),
    relations: Object.freeze([]),
  }),
});

const DETAIL_VALUES = Object.freeze({ compact: 0, standard: 1, full: 2 });

function mapProjectionRequest({
  view = "overview",
  detail = "standard",
  compact = false,
  full = false,
  maxChars = 0,
  groupBy = "",
  level = "",
  relations,
  overlays,
} = {}) {
  if (!Object.hasOwn(VIEW_PRESETS, view)) {
    throw new RangeError("view must be overview, architecture, tests, or evidence");
  }
  if (!Object.hasOwn(DETAIL_VALUES, detail)) {
    throw new RangeError("detail must be compact, standard, or full");
  }
  if (compact && full) throw new RangeError("compact and full conflict");
  if ((compact || full) && detail !== "standard") {
    throw new RangeError("detail conflicts with compact/full alias");
  }
  if (!Number.isSafeInteger(maxChars) || maxChars < 0) {
    throw new RangeError("maxChars must be a nonnegative safe integer");
  }
  const preset = VIEW_PRESETS[view];
  const selectedLevel = level || preset.level;
  let selectedGroup = groupBy || preset.groupBy;
  if (!["component", "directory", "language", "layer", "none"].includes(selectedGroup)) {
    throw new RangeError("groupBy must be component, directory, language, layer, or none");
  }
  if (!["component", "file", "symbol"].includes(selectedLevel)) {
    throw new RangeError("level must be component, file, or symbol");
  }
  if (selectedLevel === "component") {
    if (groupBy && selectedGroup !== "none") {
      throw new RangeError("component level cannot also be grouped");
    }
    selectedGroup = "none";
  }
  const definition = {
    id: `map-${view}`,
    level: selectedLevel,
    overlays: [...(overlays ?? preset.overlays)].sort(),
    relations: [
      ...(relations ?? (
        selectedLevel === "symbol" ? ["calls", "references"] : preset.relations
      )),
    ].sort(),
    select: "graph",
  };
  if (selectedGroup !== "none") definition.group_by = selectedGroup;
  return Object.freeze({
    definition: Object.freeze(definition),
    detail: DETAIL_VALUES[compact ? "compact" : (full ? "full" : detail)],
    maxChars,
  });
}

module.exports = { mapProjectionRequest };
