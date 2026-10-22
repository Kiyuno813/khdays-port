#!/usr/bin/env python3
"""Mission file ids -> names, numbers and days, from the game's own mission list (UI/cm/msl_<lang>.msi).

Reads a copy of the game extracted by extract_data.py and decompress_data.py and writes MISSIONS.md and
MISSIONS.csv. The tables hold the game's text, so they go to generated/ (not committed) by default.
  python tools/mission_table.py [--extracted data/extracted/<hash>] [--out generated/missions]
"""

from __future__ import annotations

import argparse
import csv
import struct
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# a record: u16 size, u16 ?, u16 mission file, u16 number shown, u16 first day, u16 last day ...; strings at +0x30
HEADER_WORDS = 24
STRINGS_AT = 0x30
STORY_DAY_IDS = 10000  # ids from here on are story-day scripts ("NEXT DAY")
MISSION_MODE_DAY = 400  # a first day this late keeps a mission off the story list

WORLDS = [
    "Twilight Town",
    "Wonderland",
    "Olympus Coliseum",
    "Agrabah",
    "Beast's Castle",
    "Halloween Town",
    "Neverland",
    "Castle That Never Was",
    "The World That Never Was",
    "Memory's Skyscraper",
]
HEADINGS = ("- REQUIREMENTS -", "- OBJECTIVE -", "- REQUISITOS -", "- OBJETIVO -")


def parse(path: Path) -> list[dict]:
    data = path.read_bytes()
    count = struct.unpack_from("<H", data, 0)[0]
    at, records = 2, []
    for _ in range(count):
        size = struct.unpack_from("<H", data, at)[0]
        header = struct.unpack_from(f"<{HEADER_WORDS}H", data, at)
        text = data[at + STRINGS_AT : at + size].decode("utf-16le", errors="replace")
        records.append({"header": header, "strings": text.split("\x00")})
        at += size
    if at != len(data):
        raise ValueError(f"{path}: the records do not fill the file")
    return records


def clean(text: str) -> str:
    return text.replace("\x02", "").replace("\x01", "").strip()  # highlight marks


def world_of(records: list[dict]) -> str:
    """The first world a mission's briefings name; blank when none does (never guessed)."""
    text = " ".join(clean(s) for r in records for s in r["strings"]).lower()
    return next((w for w in WORLDS if w.lower() in text), "")


def objective(record: dict) -> str:
    for s in record["strings"][1:]:
        c = clean(s)
        for heading in HEADINGS:
            if c.startswith(heading):
                return " ".join(c[len(heading) :].split())
    return ""


def days(record: dict) -> str:
    first, last = record["header"][3], record["header"][4]
    return f"{first}" if first == last else f"{first}–{last}"


def find_extracted() -> Path | None:
    found = [p for p in (ROOT / "data" / "extracted").glob("*") if (p / "nitrofs").is_dir()]
    return found[0] if len(found) == 1 else None


def table_rows(extracted: Path) -> tuple[list[list[str]], list[int]]:
    lists = extracted / "decompressed" / "UI" / "cm"
    en, es = defaultdict(list), defaultdict(list)
    for r in parse(lists / "msl_en.msi"):
        en[r["header"][1]].append(r)
    for r in parse(lists / "msl_es.msi"):
        es[r["header"][1]].append(r)
    folders = sorted(p.name for p in (extracted / "nitrofs" / "mi" / "mi").iterdir() if p.name.isdigit())

    rows = []
    for f in folders:
        k = int(f)
        listed = en.get(k)
        if not listed:
            rows.append([f, "", "", "", "", "", "", "Not in the story's mission list"])
        elif k >= STORY_DAY_IDS:
            spans = ", ".join(sorted({days(x) for x in listed}, key=lambda s: int(s.split("–")[0])))
            story = "Story days (field and cutscenes between missions)"
            rows.append([f, "", story, "", spans, "", "", 'Listed as "NEXT DAY"'])
        else:
            r = listed[0]
            note = (
                "Mission Mode only; day 400 keeps it off the story list" if r["header"][3] >= MISSION_MODE_DAY else ""
            )
            nombre = clean(es[k][0]["strings"][0]) if es.get(k) else ""
            name = clean(r["strings"][0])
            rows.append([f, str(r["header"][2]), name, nombre, days(r), world_of(listed), objective(r), note])
    extra = sorted(k for k in en if f"{k:04d}" not in folders and str(k) not in folders)
    return rows, extra


def write_tables(rows: list[list[str]], extra: list[int], out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    with open(out / "MISSIONS.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh, lineterminator="\n")
        w.writerow(
            [
                "File ID",
                "Mission #",
                "Name",
                "Nombre (ES)",
                "Day(s)",
                "World or place (named in briefing)",
                "Objective",
                "Note",
            ]
        )
        w.writerows(rows)

    md = [
        "# Mission files: names and days",
        "",
        "Built from the game's own mission list, `UI/cm/msl_en.msi` and `msl_es.msi` (the story camp's "
        "Holo-Missions list), joined to the `mi/mi/<id>` files the studio's catalog lists under Missions. Each "
        "record of the list holds the mission file it starts (halfword +2), the number the game shows (+4) and "
        "the first and last day it can be played (+6, +8; `Ov025_MissionList_HasVisibleMissionInDays`). "
        "Regenerate with `tools/mission_table.py`.",
        "",
        "- **Day(s)**: the day, or the span of days, the mission is on the list.",
        "- **World or place**: only where the mission's briefing names it; blank otherwise (not guessed).",
        "- A mission has 1-4 list records (briefing variants, such as with or without a partner, and a second "
        "entry numbered +100); the first is used here.",
        "",
        "| File ID | # | Name | Nombre (ES) | Day(s) | World or place | Objective |",
        "|---|---|---|---|---|---|---|",
    ]
    for f, num, name, nombre, d, world, obj, note in rows:
        label = name if name else f"*{note}*"
        if name and note:
            label += f" *({note})*"
        md.append(f"| {f} | {num} | {label} | {nombre} | {d} | {world} | {obj.replace('|', '/')} |")
    md += [
        "",
        'The list also has "NEXT DAY" records for story-day scripts '
        + ", ".join(map(str, extra))
        + ", which have no file of their own in `mi/mi`.",
    ]
    (out / "MISSIONS.md").write_text("\n".join(md) + "\n", encoding="utf-8", newline="\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extracted", type=Path, help="the extracted game (data/extracted/<hash>; found when only one)")
    ap.add_argument("--out", type=Path, default=ROOT / "generated" / "missions")
    args = ap.parse_args()

    extracted = args.extracted or find_extracted()
    if extracted is None:
        print("mission_table: not one game under data/extracted; name it with --extracted", file=sys.stderr)
        return 1
    if not (extracted / "decompressed" / "UI" / "cm" / "msl_en.msi").is_file():
        print(
            f"mission_table: no decompressed mission list in {extracted}: run decompress_data.py first", file=sys.stderr
        )
        return 1
    rows, extra = table_rows(extracted)
    write_tables(rows, extra, args.out)
    print(
        f"{len(rows)} files; {sum(1 for r in rows if r[1])} story missions; "
        f"{sum(1 for r in rows if r[7].startswith('Not'))} unlisted; {len({r[1] for r in rows if r[1]})} numbers "
        f"-> {args.out}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
