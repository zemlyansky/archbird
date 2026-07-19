#!/usr/bin/env python3
"""Check one-shot native calls use explicit, bounded input profiles."""

from __future__ import annotations

import ctypes

from archbird import _native


def test_engine_budget() -> None:
    defaults = _native._EngineOptions()
    _native._engine_options_init(ctypes.byref(defaults))
    budget = defaults.max_input_bytes + 1
    captured: dict[str, int] = {}
    original = _native._engine_create

    def capture(options_pointer, _engine_pointer) -> int:
        options = ctypes.cast(
            options_pointer, ctypes.POINTER(_native._EngineOptions)
        ).contents
        captured["max_input_bytes"] = options.max_input_bytes
        captured["max_values"] = options.max_values
        return 0

    try:
        _native._engine_create = capture
        _native._new_engine(budget, saved_artifact=True)
    finally:
        _native._engine_create = original

    assert captured["max_input_bytes"] > budget
    assert captured["max_values"] > defaults.max_values
    assert captured["max_values"] != budget

    try:
        _native._new_engine(budget)
    except _native.Error as error:
        assert "status=5" in str(error)
    else:
        raise AssertionError("ordinary input silently received artifact limits")

    options = _native._EngineOptions()
    assert (
        _native._engine_options_init_for_input(
            ctypes.byref(options), 1, ctypes.c_size_t(-1).value
        )
        == 5
    )


def test_largest_input_is_forwarded() -> None:
    captured: dict[str, int] = {}
    original_declare = _native._declare
    original_one_shot = _native._one_shot

    def declare(_name, _types):
        return lambda *_arguments: 0

    def one_shot(_call, *, input_budget=0, saved_artifact=False) -> bytes:
        captured["input_budget"] = input_budget
        captured["saved_artifact"] = int(saved_artifact)
        return b"ok"

    try:
        _native._declare = declare
        _native._one_shot = one_shot
        result = _native._simple_render(
            "test", [b"small", b"largest-input"], saved_artifact=True
        )
    finally:
        _native._declare = original_declare
        _native._one_shot = original_one_shot

    assert result == b"ok"
    assert captured == {
        "input_budget": len(b"largest-input"),
        "saved_artifact": 1,
    }


if __name__ == "__main__":
    test_engine_budget()
    test_largest_input_is_forwarded()
    print("frontend input budget tests passed")
