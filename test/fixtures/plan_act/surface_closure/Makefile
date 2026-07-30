CC ?= cc
PYTHON ?= python3
NODE ?= node
CFLAGS ?= -std=c11 -Wall -Wextra -Werror

# Deliberately stale: selected build/tests do not consume this Wasm export list.
WASM_EXPORTS = _core_add

.PHONY: check
check:
	mkdir -p build
	$(CC) $(CFLAGS) -Isrc src/core.c src/test_core.c -o build/test-core
	./build/test-core
	PYTHONPATH=py $(PYTHON) -m unittest discover -s py -p 'test_*.py'
	$(NODE) --test js/test_api.js
