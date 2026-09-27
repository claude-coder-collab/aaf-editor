from __future__ import annotations

import datetime
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

pytest.importorskip("aaf2")

import crosscheck as cc
from aaf2.auid import AUID
from aaf2.rational import AAFRational


def test_canon_value_maps_pyaaf2_types() -> None:
    assert cc.canon_value(AUID("0d010101-0101-2f00-060e-2b3402060101")) == "0d010101-0101-2f00-060e-2b3402060101"
    assert cc.canon_value(AAFRational(25, 1)) == {"Numerator": 25, "Denominator": 1}
    assert cc.canon_value(datetime.datetime(2005, 2, 3, 14, 47, 16)) == {
        "date": {"year": 2005, "month": 2, "day": 3},
        "time": {"hour": 14, "minute": 47, "second": 16, "fraction": 0},
    }
    assert cc.canon_value({3, 1, 2}) == [1, 2, 3]
    assert cc.canon_value(b"\x01\xff") == "01ff"


def test_diff_reports_paths_and_ignores_set_order() -> None:
    out: list[str] = []
    a = {"class": "X", "properties": {"n": 1, "list": [{"class": "A"}, {"class": "B"}]}}
    b = {"class": "X", "properties": {"n": 2, "list": [{"class": "B"}, {"class": "A"}]}}
    cc.diff(a, b, "", out)
    assert out == ["/properties/n: 1 vs 2"]
    out.clear()
    cc.diff({"Numerator": 0, "Denominator": 0}, {"Numerator": 0, "Denominator": 1}, "", out)
    assert out == []
    cc.diff({"only": 1}, {}, "", out)
    assert out == ["/only: only in aaftool"]
