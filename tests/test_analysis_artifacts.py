"""Regression-Guards fuer die committeten Analyse-Artefakte unter analysis/.

Diese Tests verhindern, dass kaputte Extraktions-Laeufe wieder eingecheckt werden
(historischer Bug: `tools/apk_class_dump.py` rief `method.get_instructions()`
statt `method.get_code().get_bc().get_instructions()` auf und schrieb
"-1 instr"-Methodenkoepfe + `DalvikCode has no attribute 'get_instructions'`
in die Artefakte).

Lauf:  ./.venv-ag/bin/python -m pytest tests -q
"""
from __future__ import annotations

from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ANALYSIS = REPO / "analysis"

# Marker, die auf einen fehlgeschlagenen Dump-Lauf hindeuten. Bewusst eng
# gefasst: APK-Strings enthalten legitime "!! "-Sequenzen, daher keine
# Substring-Suche auf "!! ", sondern zeilen-initiale Fehler-Praefixe.
FORBIDDEN_SUBSTRINGS = (
    "-1 instr",
    "DalvikCode has no attribute 'get_instructions'",
)
BINARY_SUFFIXES = {".apk", ".bin", ".elf", ".pdf"}


def _analysis_files() -> list[Path]:
    assert ANALYSIS.is_dir(), f"analysis/ fehlt: {ANALYSIS}"
    return [p for p in ANALYSIS.rglob("*") if p.is_file()]


def test_analysis_dir_exists():
    assert _analysis_files(), "analysis/ enthaelt keine Dateien"


def test_no_broken_dump_markers_in_analysis():
    offenders: list[str] = []
    for path in _analysis_files():
        if path.suffix in BINARY_SUFFIXES:
            continue
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as exc:  # pragma: no cover
            offenders.append(f"{path.relative_to(REPO)}: unlesbar ({exc})")
            continue
        for i, line in enumerate(lines, 1):
            hit = any(m in line for m in FORBIDDEN_SUBSTRINGS)
            if not hit:
                # exakt die Fehler-Zeilen des Dump-Tools, ohne APK-Strings zu treffen
                hit = line.startswith("    !! ") or (
                    line.startswith("--- ") and line.rstrip().endswith("(FEHLER)")
                )
            if hit:
                offenders.append(f"{path.relative_to(REPO)}:{i}: {line.strip()[:120]}")
    assert not offenders, "kaputte Class-Dump-Artefakte gefunden:\n" + "\n".join(
        offenders[:20]
    )


def test_no_err_artifacts_in_analysis():
    errs = [p.name for p in ANALYSIS.rglob("*.err")]
    assert not errs, f"Fehler-Logs gehoeren nicht ins Repo: {errs}"
