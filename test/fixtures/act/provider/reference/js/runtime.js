function add(left, right) {
  return wasm._core_sum(left, right)
}

module.exports = { add }
