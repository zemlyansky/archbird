OUT = build
SRC := src
MORE = extra
MORE += generated
WASM_EXPORTS = _poly_add _poly_missing _poly_ignore

.PHONY: all clean
all: native browser | order-only
	cc $(SRC)/core.c -o $(OUT)/libexample.so
	cp ./js/src/index.js $(OUT)/bundle.js

ifeq ($(MODE),debug)
native: $(MORE)
	cc src/native.c -o build/native
else
native: release
	cc src/release.c -o build/native
endif

browser:
	node tools/build.js --out=build/browser.js

clean:
	rm -rf build
