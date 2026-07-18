"""Optional Open Knowledge Format syntax adapter."""

from .parser import okf_query_input, parse_okf_bundle


_READER_EXPORTS = {
    "OKFDocument",
    "OKFIndex",
    "index_okf_bundle",
    "okf_has_errors",
    "query_okf",
    "render_okf_json",
    "render_okf_markdown",
}


def __getattr__(name):
    if name not in _READER_EXPORTS:
        raise AttributeError(name)
    from . import reader

    return getattr(reader, name)

__all__ = [
    "parse_okf_bundle",
    "okf_query_input",
    "OKFDocument",
    "OKFIndex",
    "index_okf_bundle",
    "okf_has_errors",
    "query_okf",
    "render_okf_json",
    "render_okf_markdown",
]
