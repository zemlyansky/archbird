function add(left, right) {
  return wasm._core_add(left, right)
}

module.exports = { add }
