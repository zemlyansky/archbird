const wasm = globalThis.wasm

const ops = {
  add(a, b) {
    return wasm._core_add(a, b)
  },
  nested: {
    mul(a, b) {
      return wasm._core_mul(a, b)
    }
  }
}

function add(a, b) {
  return ops.add(a, b)
}

module.exports = { add, ops }
