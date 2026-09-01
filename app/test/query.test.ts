import assert from "node:assert/strict";
import test from "node:test";

import { queryRetrievalHits } from "../src/artifacts/query";

test("Query retrieval exposes ranked entity evidence", () => {
  const hits = queryRetrievalHits({
    query: {
      retrieval: {
        hits: [{
          kind: "symbol",
          line: 9,
          name: "Client",
          path: "pkg/api.py",
          reasons: [{
            field: "symbol.documentation",
            match: "exact-token",
            term: "disposal",
            value: "Dispose caller resources.",
          }],
          score: 123,
          symbol_kind: "class",
        }],
      },
    },
  });
  assert.deepEqual(hits, [{
    kind: "symbol",
    line: 9,
    name: "Client",
    path: "pkg/api.py",
    reasons: [{
      field: "symbol.documentation",
      match: "exact-token",
      term: "disposal",
      value: "Dispose caller resources.",
    }],
    score: 123,
    symbolKind: "class",
  }]);
});

test("Query retrieval rejects malformed hit evidence", () => {
  assert.throws(
    () => queryRetrievalHits({
      query: { retrieval: { hits: [{ kind: "file", reasons: [] }] } },
    }),
    /retrieval hit 0 name/i,
  );
});

test("Query retrieval accepts pathless component identities", () => {
  assert.deepEqual(queryRetrievalHits({
    query: {
      retrieval: {
        hits: [{ kind: "component", name: "runtime", reasons: [], score: 4 }],
      },
    },
  }), [{ kind: "component", name: "runtime", reasons: [], score: 4 }]);
});
