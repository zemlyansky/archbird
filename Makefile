PYTHON ?= python
NODE ?= node
NPM ?= npm
CMAKE ?= cmake
CLANG ?= clang
EMCMAKE ?= emcmake
CLANG_FORMAT ?= clang-format
CPPCHECK ?= cppcheck
CPPCHECK_JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
ESBUILD ?= esbuild
AUDITWHEEL ?= auditwheel
TWINE ?= twine
MANYLINUX_PLATFORM ?= manylinux_2_17_x86_64
RELEASE_SOURCE_DATE_EPOCH ?= 946684800
NATIVE_BUILD ?= build/native
NATIVE_ASAN_BUILD ?= build/native-asan
NATIVE_WARNING_BUILD ?= build/native-warnings
NATIVE_WASM_BUILD ?= build/wasm
NATIVE_FUZZ_BUILD ?= build/fuzz
NATIVE_RELEASE_BUILD ?= build/release-native
CORE_BUILD ?= build
RELEASE_DIR ?= $(CURDIR)/build/release
RELEASE_TMP ?= $(CURDIR)/build/release-tmp
BUILD_TMP ?= $(CURDIR)/build/tmp
EMSCRIPTEN_CACHE ?= $(CURDIR)/build/emscripten-cache
PYTHON_NATIVE := $(CURDIR)/py/archbird/_native.py
NODE_NATIVE := $(CURDIR)/js/build/Release/_native.node
PY_RELEASE_RAW ?= $(CURDIR)/build/release-py-raw
PY_RAW_WHEEL = $(PY_RELEASE_RAW)/archbird-0.0.1-cp310-cp310-linux_x86_64.whl
PY_MANYLINUX_WHEEL = $(RELEASE_DIR)/archbird-0.0.1-cp310-cp310-manylinux2014_x86_64.manylinux_2_17_x86_64.whl
PY_SDIST = $(RELEASE_DIR)/archbird-0.0.1.tar.gz
JS_PACKAGE = $(RELEASE_DIR)/archbird-0.0.1.tgz
JS_RELEASE_SMOKE = $(CURDIR)/build/release-node-smoke
JS_BROWSER_SMOKE = $(CURDIR)/build/release-browser-smoke
APP_BROWSER_SMOKE = $(CURDIR)/build/app-browser
PY_RELEASE_SMOKE = $(CURDIR)/build/release-python-smoke
PY_SDIST_SMOKE = $(CURDIR)/build/release-python-sdist-smoke
NATIVE_C_FILES = $(shell rg --files include src bindings test | rg '\.[ch]$$' | \
	rg -v '^test/fixtures/' | sort)
NATIVE_CORE_C_FILES = $(shell rg --files src | rg '\.c$$' | sort)
NATIVE_TEST_C_FILES = $(shell rg --files bindings test | rg '\.c$$' | \
	rg -v '^test/fixtures/' | sort)
NATIVE_INCLUDE_FLAGS = -Iinclude -Isrc -Isrc/api -Isrc/base -Isrc/evidence -Isrc/map \
	-Isrc/verify -Isrc/act -Isrc/interchange/graph \
	-Isrc/interchange/okf -Isrc/interchange/reports -Ivendor/yyjson/src \
	-I$(NATIVE_BUILD)/vendor/pcre2 -Ivendor/pcre2/src \
	-Isrc/evidence/syntax/tree_sitter \
	-Ivendor/tree-sitter/lib/include -Ivendor/tree-sitter/lib/src \
	-Ivendor/tree-sitter-c/src
.PHONY: test verify evaluation-test build-c build-py editable-install test-py build-js test-js app-test app-live-test app-py-live-test app-browser-test native-configure native-build native-test native-sanitize \
	native-warnings native-wasm-smoke native-fuzz-smoke native-json-corpus native-sha256-vectors native-analyze \
	native-boundaries release-py release-js release clean \
	release-check

test: evaluation-test native-test test-py test-js app-test app-live-test app-py-live-test

evaluation-test:
	$(PYTHON) test/test_evaluation.py

app-test: export TMPDIR := $(BUILD_TMP)
app-test: native-wasm-smoke
	cd app && $(NPM) ci --ignore-scripts --no-audit --no-fund
	cd app && $(NPM) test
	cd app && $(NPM) run build

app-live-test: app-test build-js
	ARCHBIRD_ENGINE=native \
	ARCHBIRD_NATIVE_ADDON=$(NODE_NATIVE) \
		$(NODE) test/test_live_server.js test/fixtures/map_base app/dist \
		$(CURDIR)/build/live-server-test

app-py-live-test: app-test build-py
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_python_live_repository.py \
		app/dist test/fixtures/map_base \
		$(CURDIR)/build/python-live-repository-test
	PYTHONPATH=$(CURDIR)/py $(NODE) test/test_python_live_server.js \
		$(PYTHON) test/fixtures/map_base app/dist \
		$(CURDIR)/build/python-live-server-test

app-browser-test: app-test build-js
	command mkdir -p $(APP_BROWSER_SMOKE)
	ARCHBIRD_ENGINE=native \
	ARCHBIRD_NATIVE_ADDON=$(NODE_NATIVE) \
		$(NODE) js/src/cli.js \
		--config test/fixtures/map_base/archbird.json \
		--root test/fixtures/map_base --format json \
		--output $(APP_BROWSER_SMOKE)/map.json --check
	ARCHBIRD_ENGINE=native \
	ARCHBIRD_NATIVE_ADDON=$(NODE_NATIVE) \
		$(NODE) js/src/cli.js export json \
		--map test/fixtures/report_query.json --view symbols --max-nodes 0 \
		--output $(APP_BROWSER_SMOKE)/graph.json
	$(NODE) test/run_app_browser.js app/dist \
		$(APP_BROWSER_SMOKE)/graph.json $(APP_BROWSER_SMOKE)/map.json \
		test/fixtures/map_base \
		$(APP_BROWSER_SMOKE)/app.png
	ARCHBIRD_ENGINE=native \
	ARCHBIRD_NATIVE_ADDON=$(NODE_NATIVE) \
		$(NODE) test/run_live_app_browser.js app/dist test/fixtures/map_base \
		$(APP_BROWSER_SMOKE)/live $(APP_BROWSER_SMOKE)/live-app.png

build-c: export TMPDIR := $(BUILD_TMP)
build-c:
	command mkdir -p $(BUILD_TMP)
	command $(CMAKE) -S . -B $(CORE_BUILD) -DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DBUILD_TESTING=OFF -DARCHBIRD_BUILD_PYTHON=OFF \
		-DARCHBIRD_BUILD_NODE=OFF -DARCHBIRD_BUILD_SHARED=ON
	command $(CMAKE) --build $(CORE_BUILD) --target archbird_shared --parallel

build-py: export TMPDIR := $(BUILD_TMP)
build-py: build-c
	PYTHONPATH=$(CURDIR)/py \
		$(PYTHON) tools/check_source_frontend.py python

editable-install: export TMPDIR := $(BUILD_TMP)
editable-install: build-py
	ARCHBIRD_EDITABLE_SHARED=1 $(PYTHON) -m pip install --no-deps \
		--editable $(CURDIR)/py
	cd / && $(PYTHON) -c \
		"import archbird._native as n; assert n.__file__.endswith('_native.py'), n.__file__; print(n.__file__)"

test-py: build-py
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_coverage_observations.py
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_frontend_input_budget.py
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_map_report_scaling.py
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_python_provider_applicability.py
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_syntax_recovery.py
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_cli_progress.py $(CURDIR)
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_readme_examples.py
	PYTHONPATH=$(CURDIR)/py $(PYTHON) py/tests/test_repository.py \
		$(PYTHON_NATIVE) $(CURDIR)
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_scip_python_host.py \
		$(PYTHON_NATIVE) $(CURDIR)
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_map_correctness.py \
		$(PYTHON_NATIVE) $(CURDIR) $(CURDIR)/test/fixtures/map_correctness
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_compile_commands.py \
		$(PYTHON_NATIVE)
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_ecmascript_modules.py \
		$(PYTHON_NATIVE)
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_ecmascript_identity.py \
		$(PYTHON_NATIVE)
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_verify_source_lock.py \
		$(PYTHON_NATIVE)
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_verify_recipes.py \
		$(PYTHON_NATIVE) $(CURDIR)
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_verify_debug.py \
		$(PYTHON_NATIVE) $(CURDIR)
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_verify_membership.py \
		$(PYTHON_NATIVE) $(CURDIR)
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_freshness.py $(PYTHON_NATIVE)
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_fuzz_seeds.py \
		$(PYTHON_NATIVE) $(CURDIR)/test/fuzz/corpus
	$(PYTHON) test/test_self_host_policy.py

build-js: export TMPDIR := $(BUILD_TMP)
build-js:
	command mkdir -p $(BUILD_TMP)
	$(PYTHON) tools/sync_csrc.py node
	cd js && $(NPM) run build:native
	$(PYTHON) tools/check_source_frontend.py node --node $(NODE) \
		--addon $(NODE_NATIVE)

test-js: build-js build-py
	ARCHBIRD_ENGINE=native ARCHBIRD_NATIVE_ADDON=$(NODE_NATIVE) \
		$(NODE) test/test_coverage_observations.js js/src $(CURDIR) $(NODE_NATIVE)
	ARCHBIRD_ENGINE=native ARCHBIRD_NATIVE_ADDON=$(NODE_NATIVE) \
		$(NODE) --expose-gc js/test/test_frontend.js $(NODE_NATIVE) $(CURDIR)
	$(NODE) test/test_cli_progress.js js/src/cli.js $(CURDIR) $(NODE_NATIVE)
	$(NODE) test/test_readme_examples.js $(CURDIR) js/src/cli.js $(NODE_NATIVE)
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_git_diff_cli.py \
		$(CURDIR) $(NODE) $(NODE_NATIVE)
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_query_verification_overlay.py \
		$(CURDIR) $(NODE) $(NODE_NATIVE)
	PYTHONPATH=$(CURDIR)/py $(PYTHON) test/test_resolution_frontend_parity.py \
		$(PYTHON_NATIVE) $(NODE) $(NODE_NATIVE) $(CURDIR)

native-configure:
	command mkdir -p $(BUILD_TMP)
	TMPDIR=$(BUILD_TMP) \
	command $(CMAKE) -S . -B $(NATIVE_BUILD) -DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DARCHBIRD_REFERENCE_ROOT= -DARCHBIRD_BUILD_PYTHON=OFF \
		-DARCHBIRD_BUILD_NODE=OFF -DARCHBIRD_BUILD_SHARED=ON

native-build: native-configure
	TMPDIR=$(BUILD_TMP) command $(CMAKE) --build $(NATIVE_BUILD) --parallel

native-test: native-build
	TMPDIR=$(BUILD_TMP) ctest --test-dir $(NATIVE_BUILD) --output-on-failure

native-json-corpus: native-build
	@test -n "$(JSON_TEST_SUITE_ROOT)" || { \
		echo "JSON_TEST_SUITE_ROOT must name the pinned JSONTestSuite checkout" >&2; \
		exit 2; \
	}
	JSON_TEST_SUITE_ROOT=$(JSON_TEST_SUITE_ROOT) \
		$(PYTHON) test/test_json_corpus.py \
		$(NATIVE_BUILD)/archbird_native_json

native-sha256-vectors: native-build
	@test -n "$(NIST_SHA256_ROOT)" || { \
		echo "NIST_SHA256_ROOT must name the extracted NIST SHA-256 vectors" >&2; \
		exit 2; \
	}
	NIST_SHA256_ROOT=$(NIST_SHA256_ROOT) \
		$(PYTHON) test/test_sha256_vectors.py \
		$(NATIVE_BUILD)/archbird_native_hash

native-sanitize:
	command mkdir -p $(BUILD_TMP)
	TMPDIR=$(BUILD_TMP) \
	command $(CMAKE) -S . -B $(NATIVE_ASAN_BUILD) \
		-DCMAKE_BUILD_TYPE=Debug -DARCHBIRD_ENABLE_SANITIZERS=ON \
		-DARCHBIRD_REFERENCE_ROOT= \
		-DARCHBIRD_BUILD_PYTHON=OFF -DARCHBIRD_BUILD_NODE=OFF \
		-DARCHBIRD_BUILD_SHARED=OFF
	TMPDIR=$(BUILD_TMP) command $(CMAKE) --build $(NATIVE_ASAN_BUILD) --parallel
	TMPDIR=$(BUILD_TMP) ctest --test-dir $(NATIVE_ASAN_BUILD) --output-on-failure

native-warnings:
	command mkdir -p $(BUILD_TMP)
	TMPDIR=$(BUILD_TMP) \
	command $(CMAKE) -S . -B $(NATIVE_WARNING_BUILD) \
		-DCMAKE_BUILD_TYPE=Release -DARCHBIRD_WARNINGS_AS_ERRORS=ON \
		-DARCHBIRD_REFERENCE_ROOT= -DARCHBIRD_BUILD_PYTHON=OFF \
		-DARCHBIRD_BUILD_NODE=OFF -DARCHBIRD_BUILD_SHARED=OFF
	TMPDIR=$(BUILD_TMP) command $(CMAKE) --build $(NATIVE_WARNING_BUILD) --clean-first --parallel

native-wasm-smoke:
	command mkdir -p $(BUILD_TMP) $(EMSCRIPTEN_CACHE)
	EM_CACHE=$(EMSCRIPTEN_CACHE) TMPDIR=$(BUILD_TMP) \
	command $(EMCMAKE) $(CMAKE) -S . -B $(NATIVE_WASM_BUILD) \
		-DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release \
		-DARCHBIRD_BUILD_PYTHON=OFF -DARCHBIRD_BUILD_NODE=OFF \
		-DARCHBIRD_BUILD_SHARED=OFF
	EM_CACHE=$(EMSCRIPTEN_CACHE) TMPDIR=$(BUILD_TMP) \
		command $(CMAKE) --build $(NATIVE_WASM_BUILD) --parallel

native-fuzz-smoke:
	command mkdir -p $(BUILD_TMP)
	TMPDIR=$(BUILD_TMP) \
	command $(CMAKE) -S . -B $(NATIVE_FUZZ_BUILD) \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=$(CLANG) \
		-DARCHBIRD_ENABLE_FUZZERS=ON -DARCHBIRD_BUILD_PYTHON=OFF \
		-DARCHBIRD_BUILD_NODE=OFF -DARCHBIRD_BUILD_SHARED=OFF \
		-DBUILD_TESTING=OFF
	TMPDIR=$(BUILD_TMP) command $(CMAKE) --build $(NATIVE_FUZZ_BUILD) \
		--target archbird_fuzz_smoke --parallel

native-analyze: native-configure
	command $(CLANG_FORMAT) --dry-run --Werror $(NATIVE_C_FILES)
	$(PYTHON) test/test_allocator_boundary.py
	command $(CPPCHECK) -j $(CPPCHECK_JOBS) --std=c11 \
		--enable=warning,performance,portability --error-exitcode=1 \
		--suppress=missingIncludeSystem --suppress='*:*vendor/yyjson/*' \
		--suppress=missingReturn:bindings/python.c \
		--suppress=invalidPrintfArgType_s:test/fuzz/fuzz_lexical.c \
		$(NATIVE_INCLUDE_FLAGS) $(NATIVE_CORE_C_FILES) $(NATIVE_TEST_C_FILES)
	$(PYTHON) test/test_json_boundary.py

native-boundaries:
	$(PYTHON) test/test_json_boundary.py

release-py: export SOURCE_DATE_EPOCH := $(RELEASE_SOURCE_DATE_EPOCH)
release-py: export TMPDIR := $(RELEASE_TMP)
release-py: app-test
	$(PYTHON) tools/sync_csrc.py python
	$(PYTHON) tools/stage_app.py python
	command rm -rf py/build
	command rm -rf $(RELEASE_TMP)
	command mkdir -p $(RELEASE_TMP)
	command rm -rf $(PY_RELEASE_RAW)
	command mkdir -p $(RELEASE_DIR) $(PY_RELEASE_RAW)
	cd py && $(PYTHON) -m build --outdir $(PY_RELEASE_RAW)
	$(AUDITWHEEL) show $(PY_RAW_WHEEL)
	command rm -f $(PY_MANYLINUX_WHEEL)
	$(AUDITWHEEL) repair --plat $(MANYLINUX_PLATFORM) \
		--wheel-dir $(RELEASE_DIR) $(PY_RAW_WHEEL)
	$(AUDITWHEEL) show $(PY_MANYLINUX_WHEEL)
	$(PYTHON) test/check_python_wheel.py \
		$(PY_MANYLINUX_WHEEL) py/archbird
	$(PYTHON) tools/canonicalize_sdist.py \
		$(PY_RELEASE_RAW)/archbird-0.0.1.tar.gz \
		--epoch $(RELEASE_SOURCE_DATE_EPOCH)
	command cp $(PY_RELEASE_RAW)/archbird-0.0.1.tar.gz $(PY_SDIST)
	$(PYTHON) tools/stage_app.py python --clean

release-js: export SOURCE_DATE_EPOCH := $(RELEASE_SOURCE_DATE_EPOCH)
release-js: export TMPDIR := $(RELEASE_TMP)
release-js: app-test
	command mkdir -p $(RELEASE_TMP)
	$(PYTHON) tools/sync_csrc.py node
	command $(CMAKE) -S . -B $(NATIVE_RELEASE_BUILD) -DBUILD_TESTING=OFF \
		-DCMAKE_BUILD_TYPE=Release -DARCHBIRD_BUILD_PYTHON=OFF \
		-DARCHBIRD_BUILD_NODE=ON -DARCHBIRD_BUILD_SHARED=OFF
	command $(CMAKE) --build $(NATIVE_RELEASE_BUILD) --target archbird_node --parallel
	command mkdir -p $(RELEASE_DIR)
	cd js && ARCHBIRD_NATIVE_ADDON=$(abspath $(NATIVE_RELEASE_BUILD)/node/_native.node) \
		ARCHBIRD_WASM_BUILD=$(abspath $(NATIVE_WASM_BUILD)/wasm) \
		node scripts/stage-native.js
	cd js && npm pack --ignore-scripts --pack-destination $(RELEASE_DIR)
	cd js && node scripts/clean-staged.js

release-check: export TMPDIR := $(BUILD_TMP)
release-check: release-py release-js
	$(TWINE) check $(PY_MANYLINUX_WHEEL) $(PY_SDIST)
	$(PYTHON) test/check_release_archives.py \
		--forbid-prefix "$(CURDIR)" \
		--forbid-prefix "$${TMPDIR:-/tmp}" \
		$(PY_MANYLINUX_WHEEL) \
		$(PY_SDIST) \
		$(JS_PACKAGE)
	command rm -rf $(PY_SDIST_SMOKE)
	command mkdir -p $(PY_SDIST_SMOKE)/wheel
	$(PYTHON) -m tarfile -e $(PY_SDIST) $(PY_SDIST_SMOKE)
	cd $(PY_SDIST_SMOKE)/archbird-0.0.1 && \
		$(PYTHON) -m build --wheel \
		--outdir $(PY_SDIST_SMOKE)/wheel
	$(PYTHON) test/check_python_wheel.py \
		$(PY_SDIST_SMOKE)/wheel/*.whl \
		$(PY_SDIST_SMOKE)/archbird-0.0.1/archbird
	command rm -rf $(PY_RELEASE_SMOKE)
	$(PYTHON) -m venv $(PY_RELEASE_SMOKE)
	$(PY_RELEASE_SMOKE)/bin/python -m pip install \
		--disable-pip-version-check --no-deps $(PY_MANYLINUX_WHEEL)
	$(PY_RELEASE_SMOKE)/bin/python test/release_python_smoke.py \
		test/fixtures/map_base/archbird.json test/fixtures/map_base
	$(NODE) test/test_python_live_server.js \
		$(PY_RELEASE_SMOKE)/bin/python test/fixtures/map_base - \
		$(CURDIR)/build/release-python-live-smoke
	command rm -rf $(CURDIR)/build/release-self-host-smoke
	$(PYTHON) test/test_self_host_slot.py $(PY_MANYLINUX_WHEEL) \
		$(CURDIR)/build/release-self-host-smoke
	command rm -rf $(JS_RELEASE_SMOKE)
	command mkdir -p $(JS_RELEASE_SMOKE)
	$(NPM) install --ignore-scripts --no-audit --no-fund \
		--prefix $(JS_RELEASE_SMOKE) $(JS_PACKAGE)
	$(NODE) test/release_node_smoke.js \
		$(JS_RELEASE_SMOKE)/node_modules/archbird \
		test/fixtures/map_base/archbird.json test/fixtures/map_base
	$(NODE) test/release_node_cli_smoke.js \
		$(JS_RELEASE_SMOKE)/node_modules/.bin/archbird \
		$(CURDIR) $(JS_RELEASE_SMOKE)/cli native
	$(NODE) test/release_node_cli_smoke.js \
		$(JS_RELEASE_SMOKE)/node_modules/.bin/archbird \
		$(CURDIR) $(JS_RELEASE_SMOKE)/cli wasm
	$(NODE) test/test_packaged_live_cli.js \
		$(JS_RELEASE_SMOKE)/node_modules/.bin/archbird \
		test/fixtures/map_base $(JS_RELEASE_SMOKE)/live
	command rm -rf $(JS_BROWSER_SMOKE)
	command mkdir -p $(JS_BROWSER_SMOKE)
	$(ESBUILD) test/browser_release_entry.js --bundle --format=iife \
		--platform=browser \
		--alias:archbird/browser=$(JS_RELEASE_SMOKE)/node_modules/archbird/src/browser.js \
		--outfile=$(JS_BROWSER_SMOKE)/bundle.js
	command cp test/browser_release.html $(JS_BROWSER_SMOKE)/index.html
	command cp $(JS_RELEASE_SMOKE)/node_modules/archbird/wasm/archbird.wasm \
		$(JS_BROWSER_SMOKE)/archbird.wasm
	$(NODE) test/run_browser_release.js $(JS_BROWSER_SMOKE)
	cd $(JS_RELEASE_SMOKE)/node_modules/archbird && \
		$(NPM) run build:native
	ARCHBIRD_ENGINE=native \
	ARCHBIRD_NATIVE_ADDON=$(JS_RELEASE_SMOKE)/node_modules/archbird/build/Release/_native.node \
		$(NODE) test/release_node_smoke.js \
		$(JS_RELEASE_SMOKE)/node_modules/archbird \
		test/fixtures/map_base/archbird.json test/fixtures/map_base

release: release-check

verify: evaluation-test native-test test-py test-js app-test native-analyze native-warnings
	$(PYTHON) -m compileall -q \
		-x '(^|/)test/(fixtures|fuzz/corpus)(/|$$)' \
		py/archbird py/tests test

clean:
	command rm -rf $(CORE_BUILD)/CMakeFiles $(CORE_BUILD)/CMakeCache.txt \
		$(CORE_BUILD)/Makefile $(CORE_BUILD)/cmake_install.cmake \
		$(CORE_BUILD)/libarchbird.so $(CORE_BUILD)/libarchbird.dylib \
		py/build py/archbird/_native*.so py/archbird/_native*.pyd \
		js/build
