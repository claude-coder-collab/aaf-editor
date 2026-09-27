from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import embed_file


def test_render_emits_bytes_and_size() -> None:
    text = embed_file.render(b"\x00\xffA", "kData", "ns")
    assert "extern const unsigned char kData[] = {\n    0x00, 0xff, 0x41\n};" in text
    assert "extern const std::size_t kDataSize = 3;" in text
    assert "namespace ns" in text
    assert "auto data() -> std::string_view" in text
    assert "// NOLINTBEGIN" in text


def test_render_empty_file_is_valid() -> None:
    assert "kDataSize = 0;" in embed_file.render(b"", "kData", "ns")


def test_main_only_rewrites_changed_output(tmp_path: Path) -> None:
    source = tmp_path / "in.html"
    source.write_bytes(b"<p>")
    out = tmp_path / "out.cpp"
    assert embed_file.main([str(source), str(out), "kHtml"]) == 0
    stamp = out.stat().st_mtime_ns
    assert embed_file.main([str(source), str(out), "kHtml"]) == 0
    assert out.stat().st_mtime_ns == stamp
