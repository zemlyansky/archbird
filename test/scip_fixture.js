"use strict";

const encoder = new TextEncoder();

function asBytes(value) {
  if (typeof value === "string") return encoder.encode(value);
  if (value instanceof Uint8Array) return value;
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  throw new TypeError("SCIP fixture fields must be strings or byte arrays");
}

function concat(parts) {
  const length = parts.reduce((total, part) => total + part.length, 0);
  const result = new Uint8Array(length);
  let offset = 0;
  for (const part of parts) {
    result.set(part, offset);
    offset += part.length;
  }
  return result;
}

function varint(value) {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new TypeError("SCIP fixture varint must be a nonnegative safe integer");
  }
  const bytes = [];
  do {
    const byte = value % 128;
    value = Math.floor(value / 128);
    bytes.push(value ? byte | 0x80 : byte);
  } while (value);
  return Uint8Array.from(bytes);
}

function fieldKey(field, wire) {
  return varint(field * 8 + wire);
}

function integerField(field, value) {
  return concat([fieldKey(field, 0), varint(value)]);
}

function bytesField(field, value) {
  const bytes = asBytes(value);
  return concat([fieldKey(field, 2), varint(bytes.length), bytes]);
}

function stringField(field, value) {
  return bytesField(field, encoder.encode(value));
}

function singleLineRange(line, start, end) {
  return concat([
    integerField(1, line),
    integerField(2, start),
    integerField(3, end),
  ]);
}

function occurrence(symbol, roles, line, start, end) {
  return concat([
    stringField(2, symbol),
    integerField(3, roles),
    bytesField(8, singleLineRange(line, start, end)),
  ]);
}

function document(path, value, text) {
  // Position encoding field 6 is deliberately absent. Tests must provide an
  // explicit reviewed fallback instead of inferring an indexer's convention.
  return concat([
    stringField(1, path),
    bytesField(2, value),
    stringField(4, "javascript"),
    stringField(5, text),
  ]);
}

function scipFixture() {
  const symbol = "scip npm fixture 1.0 fixture/add().";
  const tool = concat([
    stringField(1, "archbird-js-fixture"),
    stringField(2, "1.0"),
  ]);
  const metadata = concat([
    bytesField(2, tool),
    integerField(4, 1),
  ]);
  return concat([
    bytesField(1, metadata),
    bytesField(2, document("src/defs.js", occurrence(symbol, 1, 0, 16, 19),
      "export function add(a, b) { return a + b; }\n")),
    bytesField(2, document("src/use.js", occurrence(symbol, 8, 0, 9, 12),
      'import { add } from "./defs.js";\nexport const value = add(1, 2);\n')),
  ]);
}

module.exports = { scipFixture };
