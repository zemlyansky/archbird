"use strict";

const assert = require("node:assert/strict");
const test = require("node:test");
const { add } = require("./runtime");

test("add uses the current core API", () => {
  const wasm = {
    _core_sum(left, right) {
      return left + right;
    },
  };
  assert.equal(add(wasm, 2, 3), 5);
});
