"use strict";

const { casefold } = require("./casefold");

function utf8Compare(left, right) {
  return Buffer.compare(Buffer.from(left), Buffer.from(right));
}

function canonical(value) {
  if (Array.isArray(value)) return value.map(canonical);
  if (value && typeof value === "object") {
    return Object.fromEntries(
      Object.keys(value)
        .sort(utf8Compare)
        .map((key) => [key, canonical(value[key])]),
    );
  }
  return value;
}

function collectStrings(value, output) {
  if (typeof value === "string") {
    if (!Buffer.from(value).equals(Buffer.from(value, "ascii"))) output.add(value);
    return;
  }
  if (Array.isArray(value)) {
    for (const item of value) collectStrings(item, output);
    return;
  }
  if (value && typeof value === "object") {
    for (const [key, item] of Object.entries(value)) {
      collectStrings(key, output);
      collectStrings(item, output);
    }
  }
}

function okfNormalization(artifacts) {
  const texts = new Set();
  for (const artifact of artifacts) {
    if (!artifact.length) continue;
    collectStrings(JSON.parse(Buffer.from(artifact).toString("utf8")), texts);
  }
  if (!texts.size) return Buffer.alloc(0);
  const rows = [...texts]
    .sort(utf8Compare)
    .map((text) => ({
      casefold: casefold(text),
      slug_ascii: text.normalize("NFKD").replace(/[^\x00-\x7f]/g, ""),
      text,
    }));
  return Buffer.from(
    JSON.stringify(
      canonical({
        artifact: "okf-text-normalization",
        rows,
        schema_version: 1,
      }),
    ),
  );
}

module.exports = { okfNormalization };
