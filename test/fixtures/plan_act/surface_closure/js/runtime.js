"use strict";

function add(wasm, left, right) {
  return wasm._core_sum(left, right);
}

module.exports = { add };
