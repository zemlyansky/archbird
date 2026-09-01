import assert from "node:assert/strict";
import test from "node:test";
import {
  documentationPreview,
  symbolDocumentation,
} from "../src/graph/documentation";

const documentation = {
  kind: "jsdoc",
  span: { end: 84, start: 0 },
  text: "Return the supplied value.\n\nThis detail remains available.",
};

test("symbol documentation preserves exact text and a source span", () => {
  assert.deepEqual(symbolDocumentation(documentation), documentation);
});

test("symbol documentation uses only the first paragraph in graph hover", () => {
  assert.equal(documentationPreview(documentation), "Return the supplied value.");
});

test("symbol documentation rejects malformed values", () => {
  assert.equal(
    symbolDocumentation({ kind: "jsdoc", text: "", span: null }),
    null,
  );
  assert.equal(documentationPreview("[object Object]"), "");
});
