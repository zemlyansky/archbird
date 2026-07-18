.PHONY: build bundle test verify

WASM_PREFIX = _core
WASM_EXPORTS = $(WASM_PREFIX)_add
WASM_EXPORTS += $(WASM_PREFIX)_mul
WASM_EXPORTS := $(WASM_EXPORTS) $(WASM_PREFIX)_unused

build:
	cc src/core.c -o build/sample

bundle: dist/sample.js

dist/sample.js: js/index.js js/runtime.js
	esbuild js/index.js --outfile=dist/sample.js

test: build
	./build/sample-test

verify: test

check-all: verify

ifeq ($(ENABLE_GPU), 1)
test-gpu:
	./build/sample-test --gpu
endif
