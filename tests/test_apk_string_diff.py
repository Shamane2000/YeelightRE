"""Smoke-Tests fuer tools/apk_string_diff.py (const-String-Versionsdiff).

Lauf:  ./.venv-ag/bin/python -m pytest tests -q
"""
from __future__ import annotations

import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

import apk_string_diff as asd  # noqa: E402


def _write(path: Path, items: list[str]) -> None:
    path.write_text("\n".join(items) + "\n", encoding="utf-8")


def test_diff_reports_both_directions(tmp_path):
    alt = tmp_path / "alt.txt"
    neu = tmp_path / "neu.txt"
    out = tmp_path / "diff.md"
    _write(alt, ["miIO.config_router", "SmartConfig", "only-old-flag"])
    _write(neu, ["miIO.config_router", "bind_key", "only-new-flag"])

    argv = sys.argv
    sys.argv = ["apk_string_diff.py", str(alt), str(neu), "alt", "neu", str(out)]
    try:
        assert asd.main() == 0
    finally:
        sys.argv = argv

    text = out.read_text(encoding="utf-8")
    assert "- alt: 3 Strings" in text
    assert "- neu: 3 Strings" in text
    # Provisioning-Kategorie: SmartConfig nur alt, bind_key nur neu
    prov = text.split("## Provisioning/SmartConfig")[1].split("\n## ")[0]
    assert "SmartConfig" in prov
    assert "bind_key" in prov
    # gemeinsamer String darf in keinem der beiden Bloecke auftauchen
    assert "miIO.config_router" not in text


def test_diff_filters_regex_and_binary_junk(tmp_path):
    alt = tmp_path / "alt.txt"
    neu = tmp_path / "neu.txt"
    out = tmp_path / "diff.md"
    junk = "(?:(?:" + "a" * 500 + ")|x)y"
    _write(alt, [junk, "miIO.ota"])
    _write(neu, [])

    argv = sys.argv
    sys.argv = ["apk_string_diff.py", str(alt), str(neu), "alt", "neu", str(out)]
    try:
        assert asd.main() == 0
    finally:
        sys.argv = argv

    text = out.read_text(encoding="utf-8")
    assert "miIO.ota" in text
    assert junk not in text


def test_printable_regex_bounds():
    assert asd.PRINTABLE.match("miIO.config_router")
    assert not asd.PRINTABLE.match("ab")  # zu kurz
    assert not asd.PRINTABLE.match("a" * 121)  # zu lang
