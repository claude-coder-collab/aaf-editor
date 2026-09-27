#!/usr/bin/env python3
"""Embed a file in a C++ source file as `<symbol>[]` and `<symbol>Size`, plus an accessor `auto <name>() -> std::string_view` (`kIndexHtml` -> `indexHtml`)."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def render(data: bytes, symbol: str, namespace: str) -> str:
    rows = [", ".join(f"0x{b:02x}" for b in data[i : i + 16]) for i in range(0, len(data), 16)]
    body = ",\n    ".join(rows) if rows else "0x00"
    function = symbol[1].lower() + symbol[2:] if symbol.startswith("k") and len(symbol) > 1 else symbol
    return (
        "#include <cstddef>\n#include <string_view>\n\n"
        "// NOLINTBEGIN\n"
        f"namespace {namespace}\n{{\n\n"
        f"extern const unsigned char {symbol}[] = {{\n    {body}\n}};\n"
        f"extern const std::size_t {symbol}Size = {len(data)};\n\n"
        f"auto {function}() -> std::string_view\n{{\n"
        f"    return {{ reinterpret_cast<const char*>({symbol}), {symbol}Size }};\n}}\n\n"
        "}\n"
        "// NOLINTEND\n"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("symbol")
    parser.add_argument("--namespace", default="aaf::embedded")
    args = parser.parse_args(argv)
    text = render(args.input.read_bytes(), args.symbol, args.namespace)
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != text:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
