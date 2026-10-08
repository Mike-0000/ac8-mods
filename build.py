"""Package the ACE COMBAT 8 mods in this repo as Nexus Mods uploads.

    python build.py build [Mod ...]     write dist/<zip_name>-<version>-Full.zip and -ModOnly.zip
    python build.py check ZIP [ZIP ...]  run the upload rules on any zip
    python build.py import Mod [MODS_DIR]  copy a mod's files from a UE4SS Mods folder into mods/<Mod>/files

Full     Game/ + EasyAntiCheat/ to copy into the game folder: loader, UE4SS, the mod, offline settings.
ModOnly  the mod folder alone, for an existing UE4SS install.

The rules in check() are what kept files out of quarantine on the ACE COMBAT 8 Nexus page; see README.md.
"""
import argparse
import json
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
MODS_IN_GAME = "Game/Binaries/Win64/ue4ss/Mods"
DEFAULT_MODS_DIR = Path(r"D:\SteamLibrary\steamapps\common\ACE COMBAT 8\Game\Binaries\Win64\ue4ss\Mods")

# Anything Windows runs on a double-click, or that a scanner treats as a script or installer.
BLOCKED = {"exe", "bat", "cmd", "com", "scr", "pif", "cpl", "msi", "msp", "ps1", "psm1", "psd1", "vbs", "vbe",
           "js", "jse", "wsf", "wsh", "hta", "lnk", "reg", "jar", "py", "sh", "sys", "drv", "ocx"}
ARCHIVES = {"zip", "rar", "7z", "tar", "gz", "bz2", "xz", "cab", "iso"}
# Written by a mod while the game runs; never shipped, and never copied back by import.
RUNTIME_SUFFIXES = (".log", ".new", ".tmp")
ZIP_DATE = (2026, 1, 1, 0, 0, 0)


def check(zip_path):
    """Return the list of upload-rule violations in a zip."""
    problems = []
    with zipfile.ZipFile(zip_path) as archive:
        for info in archive.infolist():
            if info.is_dir():
                continue
            name = info.filename
            parts = Path(name).name.lower().split(".")
            ext = parts[-1] if len(parts) > 1 else ""
            head = archive.open(info).read(4)
            if ext in BLOCKED:
                problems.append(f"{name}: .{ext} files get an upload quarantined")
            if ext in ARCHIVES or head[:2] == b"PK" or head == b"Rar!" or head[:2] == b"7z":
                problems.append(f"{name}: archive inside the archive")
            if head[:2] == b"MZ" and ext != "dll":
                problems.append(f"{name}: Windows binary without a .dll name")
            if ext == "dll" and head[:2] != b"MZ":
                problems.append(f"{name}: named .dll but is not a Windows binary")
            if any(part in BLOCKED | {"dll"} for part in parts[1:-1]):
                problems.append(f"{name}: hidden extension")
            if "\\" in name or name.startswith("/") or ".." in Path(name).parts:
                problems.append(f"{name}: bad path")
    return problems


def load_mod(name):
    folder = ROOT / "mods" / name
    meta = json.loads((folder / "mod.json").read_text(encoding="utf-8"))
    meta["about"] = (folder / "about.txt").read_text(encoding="utf-8").strip()
    meta["about"] = meta["about"].replace("{source_url}", meta.get("source_url", ""))
    meta["files"] = sorted(p for p in (folder / "files").rglob("*") if p.is_file())
    meta["files_root"] = folder / "files"
    return meta


def readme(template, meta):
    text = (ROOT / "templates" / template).read_text(encoding="utf-8")
    values = dict(meta, user_files=", ".join(meta["user_files"]) or "none")
    for key in ("title", "version", "folder", "about", "user_files"):
        text = text.replace("{" + key + "}", values[key])
    return text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")


def write_zip(path, entries):
    """entries: (name in zip, bytes). Fixed dates and order, so the same input gives the same file."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(entries):
            archive.writestr(zipfile.ZipInfo(name, ZIP_DATE), data, zipfile.ZIP_DEFLATED, 9)


def build(name, dist):
    meta = load_mod(name)
    mod = [(f"{meta['folder']}/{p.relative_to(meta['files_root']).as_posix()}", p.read_bytes()) for p in meta["files"]]
    for entry, _ in mod:
        if entry.lower().endswith(RUNTIME_SUFFIXES) or Path(entry).name in meta["user_files"]:
            raise SystemExit(f"{name}: {entry} is a log or the player's own settings; remove it from files/")
    # enabled.txt makes UE4SS load the folder without a mods.txt entry, so no shared list is touched.
    mod.append((f"{meta['folder']}/enabled.txt", b""))
    runtime = ROOT / "runtime"
    full = [(p.relative_to(runtime).as_posix(), p.read_bytes()) for p in runtime.rglob("*") if p.is_file()]
    full += [(f"{MODS_IN_GAME}/{entry}", data) for entry, data in mod]
    full.append(("README.txt", readme("readme-full.txt", meta)))
    mod.append(("README.txt", readme("readme-modonly.txt", meta)))
    built = []
    for kind, entries in (("Full", full), ("ModOnly", mod)):
        path = dist / f"{meta['zip_name']}-{meta['version']}-{kind}.zip"
        write_zip(path, entries)
        problems = check(path)
        if problems:
            path.unlink()
            raise SystemExit(f"{path.name} breaks the upload rules:\n  " + "\n  ".join(problems))
        built.append(path)
        print(f"{path}  {path.stat().st_size:,} bytes, {len(entries)} files")
    return built


def import_mod(name, mods_dir):
    """Refresh mods/<name>/files from an installed copy, leaving logs and the player's settings behind."""
    meta = load_mod(name)
    source = Path(mods_dir) / meta["folder"]
    if not source.is_dir():
        raise SystemExit(f"{source} does not exist")
    target = meta["files_root"]
    shutil.rmtree(target)
    for path in sorted(source.rglob("*")):
        rel = path.relative_to(source)
        skip = (rel.parts[0].lower() in ("hook", "src") or path.name in meta["user_files"] + ["enabled.txt"]
                or path.name.lower().endswith(RUNTIME_SUFFIXES))
        if path.is_file() and not skip:
            (target / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target / rel)
            print(f"  {rel}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("build")
    p.add_argument("mods", nargs="*")
    p = sub.add_parser("check")
    p.add_argument("zips", nargs="+")
    p = sub.add_parser("import")
    p.add_argument("mod")
    p.add_argument("mods_dir", nargs="?", default=DEFAULT_MODS_DIR)
    args = parser.parse_args()

    if args.command == "build":
        dist = ROOT / "dist"
        dist.mkdir(exist_ok=True)
        for name in args.mods or sorted(p.name for p in (ROOT / "mods").iterdir() if p.is_dir()):
            build(name, dist)
    elif args.command == "check":
        failed = False
        for zip_path in args.zips:
            problems = check(zip_path)
            failed = failed or bool(problems)
            print(f"{zip_path}: " + ("OK" if not problems else "".join(f"\n  {p}" for p in problems)))
        sys.exit(1 if failed else 0)
    else:
        import_mod(args.mod, args.mods_dir)


if __name__ == "__main__":
    main()
