#!/usr/bin/env python3
"""Check one-shot native calls derive parser limits from their inputs."""

from __future__ import annotations

import ctypes

from archbird import _native


def test_engine_budget() -> None:
    defaults = _native._EngineOptions()
    _native._engine_options_init(ctypes.byref(defaults))
    budget = max(defaults.max_input_bytes, defaults.max_values) + 1
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
        _native._new_engine(budget)
    finally:
        _native._engine_create = original

    assert captured == {
        "max_input_bytes": budget,
        "max_values": budget,
    }


def test_largest_input_is_forwarded() -> None:
    captured: dict[str, int] = {}
    original_declare = _native._declare
    original_one_shot = _native._one_shot

    def declare(_name, _types):
        return lambda *_arguments: 0

    def one_shot(_call, *, input_budget=0) -> bytes:
        captured["input_budget"] = input_budget
        return b"ok"

    try:
        _native._declare = declare
        _native._one_shot = one_shot
        result = _native._simple_render("test", [b"small", b"largest-input"])
    finally:
        _native._declare = original_declare
        _native._one_shot = original_one_shot

    assert result == b"ok"
    assert captured == {"input_budget": len(b"largest-input")}


if __name__ == "__main__":
    test_engine_budget()
    test_largest_input_is_forwarded()
    print("frontend input budget tests passed")
