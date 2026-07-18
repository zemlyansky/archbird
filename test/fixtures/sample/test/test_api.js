const { add } = require('../js/index.js')

test('api:add', () => {
  expect(add(1, 2)).toBe(3)
})
