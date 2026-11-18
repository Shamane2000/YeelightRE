"""Deterministic checks on the fw76 live-probe transcript (task t_39c30d14, AC1).

The transcript `analysis/lan_probe_fw76_192.168.1.176.txt` is the AC1 proof artifact:
it must contain the real device answer (fw_ver=76, model=color) and the result of the
idempotent `set_ps["cfg_lan_ctrl","1"]` probe. These tests guard against the artifact
being replaced by an empty/negative transcript without anyone noticing.
"""
import re
from pathlib import Path

TRANSCRIPT = Path(__file__).resolve().parents[1] / "analysis" / "lan_probe_fw76_192.168.1.176.txt"


def _text():
    return TRANSCRIPT.read_text(encoding="utf-8")


def test_transcript_exists_and_is_positive():
    t = _text()
    assert "POSITIV" in t, "Transkript muss den Live-Erfolg ausweisen"
    assert "NEGATIV" not in t.splitlines()[0]


def test_get_prop_reports_fw76_color():
    t = _text()
    assert re.search(r'get_prop\[\'fw_ver\'.*?"76", "color", "1", "on"', t), \
        "get_prop muss fw_ver=76 und model=color zeigen"


def test_set_ps_cfg_lan_ctrl_was_rejected():
    t = _text()
    line = [ln for ln in t.splitlines() if "set_ps[cfg_lan_ctrl,1]" in ln]
    assert line, "set_ps[cfg_lan_ctrl,1]-Ergebniszeile fehlt"
    assert "method not supported" in line[0]
    assert 'readback get_prop          {"id": 91, "result": ["1"]}' in t, \
        "lan_ctrl muss nach dem Schreibversuch unveraendert 1 sein"


def test_reproducible_probe_command_is_documented():
    t = _text()
    assert "tools/lan_method_probe.py 192.168.1.176" in t
    assert "34:ce:00:90:04:86" in t, "MAC/OUI-Beleg fehlt"
    assert "yeelight://192.168.1.176:55443" in t, "SSDP-Beleg fehlt"
