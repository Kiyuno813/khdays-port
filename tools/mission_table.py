#!/usr/bin/env python3
"""Mission file ids -> names, numbers, days, worlds and rooms.

Names, numbers and days come from the game's own mission list (UI/cm/msl_<lang>.msi). The worlds and rooms a
mission plays in come from its scripts as khdays-studio reads them (`--mission-info`: the steps
Ov002_ScriptCmd_LoadWorldLinkResources and Ov002_ScriptCmd_LoadPeerIntoSlot), the worlds' names from the
game's UI/cm/str/world_id_<lang>.s, the rooms' ids from `--world-info`, their model names from the graphics
dump (saves/studio.ini's graphics_dump) and their KINGDOM HEARTS II maps from studio/data/kh2_rooms.txt.

Reads a copy of the game extracted by extract_data.py and decompress_data.py and writes MISSIONS.md and
MISSIONS.csv. The tables hold the game's text, so they go to generated/ (not committed) by default.
  python tools/mission_table.py [--extracted data/extracted/<hash>] [--out generated/missions]
                                [--studio build/studio/release/khdays-studio.exe] [--dump <graphics dump>]
"""

from __future__ import annotations

import argparse
import csv
import re
import struct
import subprocess
import sys
from collections import defaultdict
from html import escape as html_escape
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def escape(text: str) -> str:
    return html_escape(text, quote=False)  # in HTML text, not attributes: quotes kept as they are


# a record: u16 size, u16 ?, u16 mission file, u16 number shown, u16 first day, u16 last day ...; strings at +0x30
HEADER_WORDS = 24
STRINGS_AT = 0x30
STORY_DAY_IDS = 10000  # ids from here on are story-day scripts ("NEXT DAY")
MISSION_MODE_DAY = 400  # a first day this late keeps a mission off the story list

# the world codes in the order of data_ov002_0207f0a4 (Ov002_GetWorldName, mi/wd/wd_<code>): the index a
# LoadWorldLinkResources step names. world_id_<lang>.s lists the worlds' names in the same order.
WORLD_CODES = ["tt", "aw", "he", "al", "eh", "nm", "hb", "zz", "pp", "bb", "tw", "pi"]
# a room's object flags for its own models (the others are doors, gates and props that open and close)
ROOM_MODEL_FLAGS = "80"
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


def parse_strings(path: Path) -> list[str]:
    """A UI string table (*.s): a u32, a u32 count, then per string a u32 size (itself included) and UTF-16."""
    data = path.read_bytes()
    count = struct.unpack_from("<I", data, 4)[0]
    at, out = 8, []
    for _ in range(count):
        size = struct.unpack_from("<I", data, at)[0]
        out.append(data[at + 4 : at + size].decode("utf-16le").rstrip("\x00"))
        at += size
    if at != len(data):
        raise ValueError(f"{path}: the strings do not fill the file")
    return out


def clean(text: str) -> str:
    return text.replace("\x02", "").replace("\x01", "").strip()  # highlight marks


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


# --- worlds and rooms, from the studio's reading of the scripts and worlds -----------------------------------


def studio_text(studio: Path, *args: str) -> str:
    # the studio reads data/extracted and saves/studio.ini from the folder it runs in
    done = subprocess.run([str(studio), *args], capture_output=True, text=True, cwd=ROOT, check=False)
    if done.returncode != 0:
        raise RuntimeError(f"{studio.name} {' '.join(args)}: {done.stderr.strip() or done.returncode}")
    return done.stdout


def mission_places(studio: Path, folder: str) -> dict[str, list[int]]:
    """The worlds a mission's scripts load (in the order first met), each with the rooms put in slots by the
    scripts that load it. A script loads its world before its rooms."""
    listing = studio_text(studio, "--mission-info", folder)
    places: dict[str, set[int]] = {}
    for script in re.findall(r"^\s+(\d+:\d+)\s+\S+\s+\d+ steps", listing, re.M):
        world = None
        for line in studio_text(studio, "--mission-info", folder, script).splitlines():
            m = re.search(r"LoadWorldLinkResources\((\d+), -?\d+\)", line)
            if m:
                index = int(m.group(1))
                world = WORLD_CODES[index] if index < len(WORLD_CODES) else f"#{index}"
                places.setdefault(world, set())
                continue
            m = re.search(r"LoadPeerIntoSlot\(-?\d+, (\d+)\)", line)
            if m and world is not None:
                places[world].add(int(m.group(1)))
    return {w: sorted(rooms) for w, rooms in places.items()}


def world_rooms(studio: Path, code: str) -> dict[int, dict]:
    """A shipped world's rooms by their place in its table: the id missions search and the archive entries of
    the room's own models."""
    rooms = {}
    for line in studio_text(studio, "--world-info", code).splitlines():
        m = re.match(r"room (\d+) id (\d+)", line)
        if m:
            objects = re.findall(r"object #(\d+) flags (\w+)", line)
            rooms[int(m.group(1))] = {
                "id": int(m.group(2)),
                "entries": [int(e) for e, flags in objects if flags == ROOM_MODEL_FLAGS],
            }
    return rooms


def kh2_pairs() -> dict[tuple[str, int], str]:
    """The KH2 map each Days room was made from (studio/data/kh2_rooms.txt: <world> <room> <map> ...)."""
    pairs = {}
    path = ROOT / "studio" / "data" / "kh2_rooms.txt"
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            words = line.split(";")[0].split()
            if len(words) >= 3 and not line.startswith("#") and words[1].isdigit():
                pairs[(words[0], int(words[1]))] = words[2]
    return pairs


def graphics_dump() -> Path | None:
    """saves/studio.ini's graphics_dump (relative to the root unless absolute), when it is there."""
    value = "saves/dump/graphics"
    ini = ROOT / "saves" / "studio.ini"
    if ini.is_file():
        for line in ini.read_text(encoding="utf-8").splitlines():
            key, _, v = line.partition("=")
            if key.strip() == "graphics_dump" and v.strip():
                value = v.strip()
    path = Path(value) if Path(value).is_absolute() else ROOT / value
    return path if path.is_dir() else None


def room_model(dump: Path | None, code: str, entries: list[int]) -> str:
    """The name of a room's first model, as the graphics dump names it (<dump>/mi/wd/wd_<code>#<entry>/mdl)."""
    if dump is None:
        return ""
    for entry in entries:
        names = sorted(p.stem for p in (dump / "mi" / "wd" / f"wd_{code}#{entry}" / "mdl").glob("*.gltf"))
        if names:
            return names[0]
    return ""


class Places:
    """Every mission's worlds and rooms, and every shipped world's rooms with their labels."""

    def __init__(self, studio: Path, extracted: Path, dump: Path | None, folders: list[str]) -> None:
        names = extracted / "decompressed" / "UI" / "cm" / "str"
        self.names = dict(zip(WORLD_CODES, parse_strings(names / "world_id_en.s")))
        self.nombres = dict(zip(WORLD_CODES, parse_strings(names / "world_id_es.s")))
        self.shipped = sorted(p.name[3:] for p in (extracted / "nitrofs" / "mi" / "wd").glob("wd_*"))
        self.rooms = {code: world_rooms(studio, code) for code in self.shipped}
        pairs = kh2_pairs()
        self.labels = {}
        for code, rooms in self.rooms.items():
            for index, room in rooms.items():
                model = room_model(dump, code, room["entries"])
                parts = [f"room {index}", f"id {room['id']}"]
                if model:
                    parts.append(model)
                if (code, index) in pairs:
                    parts.append(f"KH2 {pairs[(code, index)]}")
                self.labels[(code, index)] = " · ".join(parts)
        self.missions = {f: mission_places(studio, f) for f in folders}

    def world(self, code: str) -> str:
        return f"{self.names.get(code, code)} ({code})"

    def label(self, code: str, index: int) -> str:
        if code not in self.rooms:
            return f"room {index} (no wd_{code} in the game)"
        return self.labels.get((code, index), f"room {index} (not in wd_{code}'s table)")


# --- the tables --------------------------------------------------------------------------------------------


def table_rows(extracted: Path) -> tuple[list[list[str]], list[int], list[str]]:
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
            rows.append([f, "", "", "", "", "", "Not in the story's mission list"])
        elif k >= STORY_DAY_IDS:
            spans = ", ".join(sorted({days(x) for x in listed}, key=lambda s: int(s.split("–")[0])))
            story = "Story days (field and cutscenes between missions)"
            rows.append([f, "", story, "", spans, "", 'Listed as "NEXT DAY"'])
        else:
            r = listed[0]
            note = (
                "Mission Mode only; day 400 keeps it off the story list" if r["header"][3] >= MISSION_MODE_DAY else ""
            )
            nombre = clean(es[k][0]["strings"][0]) if es.get(k) else ""
            rows.append([f, str(r["header"][2]), clean(r["strings"][0]), nombre, days(r), objective(r), note])
    extra = sorted(k for k in en if f"{k:04d}" not in folders and str(k) not in folders)
    return rows, extra, folders


def rooms_html(places: Places, code: str, rooms: list[int]) -> str:
    """A world's dropdown: its name, then the mission's rooms in it, one a line."""
    count = f"{len(rooms)} room" + ("" if len(rooms) == 1 else "s")
    inner = "<br>".join(escape(places.label(code, i)) for i in rooms) or "no room put in a slot"
    return f"<details><summary>{escape(places.world(code))} · {count}</summary>{inner}</details>"


def world_cell(places: Places | None, f: str) -> str:
    if places is None:
        return ""
    worlds = places.missions.get(f, {})
    if not worlds:
        return "*none loaded*"
    if len(worlds) == 1:
        code, rooms = next(iter(worlds.items()))
        return rooms_html(places, code, rooms)
    # several worlds: a dropdown of worlds, each one's rooms a dropdown inside it
    inner = "".join(rooms_html(places, code, rooms) for code, rooms in worlds.items())
    return f"<details><summary>{len(worlds)} worlds</summary>{inner}</details>"


def worlds_section(places: Places, rows: list[list[str]]) -> list[str]:
    """Per shipped world a dropdown of its rooms, each room's a dropdown of the missions that use it."""
    names = {r[0]: r[2] or f"({r[6].lower()})" for r in rows}
    users: dict[tuple[str, int], list[str]] = defaultdict(list)
    missions_of: dict[str, set[str]] = defaultdict(set)
    for f, worlds in places.missions.items():
        for code, rooms in worlds.items():
            missions_of[code].add(f)
            for i in rooms:
                users[(code, i)].append(f)
    md = [
        "",
        "## Worlds and their rooms",
        "",
        "Every room of each world the game ships (`mi/wd/wd_<code>`), with the mission files whose scripts put "
        "it in a slot. Open a world, then a room.",
        "",
    ]
    for code in sorted(places.shipped, key=lambda c: WORLD_CODES.index(c) if c in WORLD_CODES else 99):
        rooms = places.rooms[code]
        md.append(
            f"<details><summary><b>{escape(places.world(code))}</b> · {escape(places.nombres.get(code, ''))} · "
            f"{len(rooms)} rooms, {len(missions_of[code])} mission files</summary>"
        )
        md.append("<ul>")
        for i in sorted(rooms):
            who = users.get((code, i), [])
            if who:
                items = "".join(f"<li>{f} {escape(names.get(f, ''))}</li>" for f in who)
                md.append(
                    f"<li><details><summary>{escape(places.label(code, i))} · {len(who)} mission files</summary>"
                    f"<ul>{items}</ul></details></li>"
                )
            else:
                md.append(f"<li>{escape(places.label(code, i))} · <i>no mission script loads it</i></li>")
        md.append("</ul>")
        md.append("</details>")
        md.append("")
    missing = sorted({c for w in places.missions.values() for c in w} - set(places.shipped))
    if missing:
        md.append(
            "Loaded by a script but not in the game (no `mi/wd/wd_<code>`): "
            + "; ".join(
                f"{escape(places.world(c))} by " + ", ".join(f for f, w in places.missions.items() if c in w)
                for c in missing
            )
            + "."
        )
    return md


def write_tables(rows: list[list[str]], extra: list[int], places: Places | None, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    with open(out / "MISSIONS.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh, lineterminator="\n")
        w.writerow(["File ID", "Mission #", "Name", "Nombre (ES)", "Day(s)", "World(s)", "Rooms", "Objective", "Note"])
        for f, num, name, nombre, d, obj, note in rows:
            worlds = places.missions.get(f, {}) if places else {}
            world_names = "; ".join(places.world(c) for c in worlds) if places else ""
            rooms = "; ".join(f"{c}: " + ", ".join(places.label(c, i) for i in rs) for c, rs in worlds.items())
            w.writerow([f, num, name, nombre, d, world_names, rooms, obj, note])

    md = [
        "# Mission files: names, days, worlds and rooms",
        "",
        "Built from the game's own mission list, `UI/cm/msl_en.msi` and `msl_es.msi` (the story camp's "
        "Holo-Missions list), joined to the `mi/mi/<id>` files the studio's catalog lists under Missions. Each "
        "record of the list holds the mission file it starts (halfword +2), the number the game shows (+4) and "
        "the first and last day it can be played (+6, +8; `Ov025_MissionList_HasVisibleMissionInDays`). "
        "Regenerate with `tools/mission_table.py`.",
        "",
        "- **Day(s)**: the day, or the span of days, the mission is on the list.",
        "- **World and rooms**: what the mission's scripts load, as khdays-studio reads them: the world of each "
        "`LoadWorldLinkResources` step (named by the game's `UI/cm/str/world_id_en.s`, in the same order) and "
        "the rooms its `LoadPeerIntoSlot` steps put in slots. Open the dropdown for the rooms; a mission in "
        "several worlds has one dropdown per world inside it.",
        "- **A room** is named by its place in the world's table (what the studio's Maps view and "
        "`--world-info` call it), its id (what scripts search), its first model's name in the graphics dump "
        "and, where known, the KINGDOM HEARTS II map it was made from (`studio/data/kh2_rooms.txt`).",
        "- A mission has 1-4 list records (briefing variants, such as with or without a partner, and a second "
        "entry numbered +100); the first is used here.",
        "",
        "| File ID | # | Name | Nombre (ES) | Day(s) | World and rooms | Objective |",
        "|---|---|---|---|---|---|---|",
    ]
    for f, num, name, nombre, d, obj, note in rows:
        label = name if name else f"*{note}*"
        if name and note:
            label += f" *({note})*"
        cell = world_cell(places, f)
        md.append(f"| {f} | {num} | {label} | {nombre} | {d} | {cell} | {obj.replace('|', '/')} |")
    md += [
        "",
        'The list also has "NEXT DAY" records for story-day scripts '
        + ", ".join(map(str, extra))
        + ", which have no file of their own in `mi/mi`.",
    ]
    if places is not None:
        md += worlds_section(places, rows)
    (out / "MISSIONS.md").write_text("\n".join(md) + "\n", encoding="utf-8", newline="\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extracted", type=Path, help="the extracted game (data/extracted/<hash>; found when only one)")
    ap.add_argument("--out", type=Path, default=ROOT / "generated" / "missions")
    exe = "khdays-studio.exe" if sys.platform == "win32" else "khdays-studio"
    ap.add_argument(
        "--studio",
        type=Path,
        default=ROOT / "build" / "studio" / "release" / exe,
        help="the khdays-studio that reads the scripts and worlds (studio/build.py makes it)",
    )
    ap.add_argument("--dump", type=Path, help="the graphics dump, for the rooms' model names (default: studio.ini's)")
    ap.add_argument("--no-worlds", action="store_true", help="leave out the worlds and rooms (no studio needed)")
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
    rows, extra, folders = table_rows(extracted)
    places = None
    if not args.no_worlds:
        if not args.studio.is_file():
            print(
                f"mission_table: no {args.studio}: build it with studio/build.py, name it with --studio, "
                "or leave the worlds out with --no-worlds",
                file=sys.stderr,
            )
            return 1
        dump = args.dump or graphics_dump()
        if dump is None:
            print("mission_table: no graphics dump: the rooms are listed without their model names", file=sys.stderr)
        places = Places(args.studio, extracted, dump, folders)
    write_tables(rows, extra, places, args.out)
    worlds = f"; {sum(1 for f in folders if places.missions.get(f))} with a world" if places else ""
    print(
        f"{len(rows)} files; {sum(1 for r in rows if r[1])} story missions; "
        f"{sum(1 for r in rows if r[6].startswith('Not'))} unlisted; {len({r[1] for r in rows if r[1]})} numbers"
        f"{worlds} -> {args.out}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
