from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import gen_model

SMALL_MODEL: gen_model.Model = {
    "classes": [
        {
            "id": "0d010101-0101-0100-060e-2b3402060101",
            "name": "InterchangeObject",
            "parent": None,
            "concrete": False,
            "properties": [
                {"id": "06010104-0101-0000-060e-2b3401010102", "name": "ObjClass", "pid": 0x0101, "type": "05010100-0000-0000-060e-2b3401040101", "optional": False, "unique": False}
            ],
        }
    ],
    "types": [
        {"id": "01010100-0000-0000-060e-2b3401040101", "name": "aafUInt8", "kind": "integer", "size": 1, "signed": False},
        {"id": "01040100-0000-0000-060e-2b3401040101", "name": "Boolean", "kind": "enumeration", "element": "01010100-0000-0000-060e-2b3401040101", "values": [[0, "False"], [1, "True"]]},
    ],
}


def test_generate_cpp_is_deterministic_and_complete() -> None:
    cpp = gen_model.generate_cpp(SMALL_MODEL)
    assert cpp == gen_model.generate_cpp(SMALL_MODEL)
    assert '"ObjClass", "05010100-0000-0000-060e-2b3401040101", 0x0101' in cpp
    assert "GenKind::enumeration" in cpp
    assert '{ "True", 1 },' in cpp
    assert "kClassProps0" in cpp


def test_committed_cpp_matches_committed_json() -> None:
    assert gen_model.main(["check"]) == 0


def test_json_matches_installed_pyaaf2() -> None:
    pytest.importorskip("aaf2")
    import json

    committed = json.loads(gen_model.JSON_PATH.read_text(encoding="utf-8"))
    assert gen_model.load_pyaaf2() == committed
