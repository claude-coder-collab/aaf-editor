from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import gen_labels

REGISTER = b"""<!-- master_commit_tag=v000000099 -->
<LabelsRegister xmlns="http://www.smpte-ra.org/schemas/400/2012"><Entries>
<Entry><UL>urn:smpte:ul:060e2b34.04010101.0d010201.01010000</UL><Kind>NODE</Kind><Name>MXF OP1a</Name><IsDeprecated>false</IsDeprecated></Entry>
<Entry><UL>urn:smpte:ul:060e2b34.04010101.0d010201.01010100</UL><Kind>LEAF</Kind><Name>OP1a  Internal</Name><IsDeprecated>false</IsDeprecated></Entry>
<Entry><UL>urn:smpte:ul:060e2b34.04010105.0d010201.01010100</UL><Kind>LEAF</Kind><Name>OP1a newer</Name><IsDeprecated>true</IsDeprecated></Entry>
<Entry><UL>urn:smpte:ul:060e2b34.04010101.01030202.01000000</UL><Kind>LEAF</Kind><Name>Picture Essence Track</Name><IsDeprecated>false</IsDeprecated></Entry>
</Entries></LabelsRegister>
"""


def test_auid_string_swaps_the_ul_halves() -> None:
    ul = gen_labels.ul_bytes("urn:smpte:ul:060e2b34.04010101.0d010201.01010100")
    assert gen_labels.auid_string(ul) == "0d010201-0101-0100-060e-2b3404010101"


def test_register_is_parsed_sorted_and_deduplicated_by_version() -> None:
    data = gen_labels.parse_register(REGISTER)
    assert data["source"]["version"] == "v000000099"
    names = [label["name"] for label in data["labels"]]
    assert "OP1a Internal" in names
    assert "OP1a newer" not in names
    assert [label["node"] for label in data["labels"] if label["name"] == "MXF OP1a"] == [True]
    keys = [gen_labels.sort_key(bytes.fromhex(label["ul"])) for label in data["labels"]]
    assert keys == sorted(keys)


def test_json_and_cpp_outputs_round_trip() -> None:
    data = gen_labels.parse_register(REGISTER)
    assert json.loads(gen_labels.dump_json(data)) == data
    cpp = gen_labels.generate_cpp(data)
    assert '{ "01030202-0100-0000-060e-2b3404010101"_auid, "Picture Essence Track", false, false },' in cpp


def test_committed_table_is_up_to_date() -> None:
    assert gen_labels.main(["check"]) == 0
