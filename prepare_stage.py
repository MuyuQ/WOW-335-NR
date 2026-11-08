"""Prepare a private, locked AIO component source in the project's scratch area."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import zipfile

PROJECT = Path(__file__).resolve().parents[2]
MODULE = Path(__file__).resolve().parent
SCRATCH = (PROJECT / "_scratch" / "dlss5-aio").resolve(strict=True)
ZIP_SHA = "4fc48ccd5e6ca144b20d47fd049cfbc9b888d4f0fb7e87847a6d72a967491fed"
EXTRA = {
    "d3d9.dll": "reshade32",
    "host64/dxgi.dll": "reshade64",
    "reshade-shaders/Shaders/ReShade.fxh": "reshade_fxh",
    "host64/nvngx_dlssnr.dll": "nr",
    "host64/nvngx_dlss.dll": "sr",
}


def sha(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def no_links(path):
    path = Path(os.path.abspath(path))
    for item in (path, *path.parents):
        try:
            info = item.lstat()
        except FileNotFoundError:
            continue
        if item.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError(f"link or reparse point in path: {item}")
    return path


def safe_member(name):
    path = PurePosixPath(name)
    if (path.is_absolute() or not path.parts or any(part in ("", ".", "..") for part in path.parts) or
            "\\" in name or ":" in name or any(ord(ch) < 32 for ch in name)):
        raise ValueError(f"unsafe package member: {name}")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("aio-zip", "reshade32", "reshade64", "reshade-fxh", "nr", "sr"):
        parser.add_argument("--" + flag, type=Path, required=True)
    parser.add_argument("--batch", type=Path, required=True, help="existing isolated DLSS test batch in _scratch")
    parser.add_argument("--name", required=True, help="new staging directory name: letters, digits, dash or underscore")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", args.name):
        parser.error("invalid staging name")
    paths = {key: no_links(getattr(args, key)) for key in ("reshade32", "reshade64", "reshade_fxh", "nr", "sr")}
    archive_path = no_links(args.aio_zip)
    if any(not path.is_file() for path in [archive_path, *paths.values()]):
        parser.error("a required source file is missing")
    if sha(archive_path) != ZIP_SHA:
        parser.error("AIO archive identity differs from the lock")
    lock = json.loads((MODULE / "payload-lock.json").read_text(encoding="utf-8"))
    members = [item for item in lock["members"] if item["source_kind"] == "stage"]
    if lock["schema"] != 1 or len(members) != 21:
        parser.error("unsupported component lock")
    for member in members:
        safe_member(member["source"])
    expected = {item["source"]: item["sha256"] for item in members}
    for relative, key in EXTRA.items():
        if relative not in expected or sha(paths[key]) != expected[relative]:
            parser.error(f"{key} identity differs from the lock")
    batch = no_links(args.batch)
    if not batch.is_dir() or not batch.is_relative_to(SCRATCH) or batch == SCRATCH:
        parser.error("batch must be an existing isolated DLSS scratch directory")
    stage_root = no_links(batch / "candidate" / "staged")
    if not stage_root.is_relative_to(batch):
        parser.error("staging directory is outside the selected batch")
    target = no_links(stage_root / args.name)
    if target.exists():
        parser.error("staging directory already exists")
    with zipfile.ZipFile(archive_path) as archive:
        names = [item.filename for item in archive.infolist() if not item.is_dir()]
        if len(names) != len(set(names)) or archive.testzip() is not None:
            parser.error("AIO archive has duplicate or invalid members")
        for relative in expected:
            if relative not in EXTRA and relative not in names:
                parser.error(f"AIO archive lacks {relative}")
        target.mkdir(parents=True, exist_ok=False)
        try:
            for relative, expected_hash in expected.items():
                member = safe_member(relative)
                destination = no_links(target.joinpath(*member.parts))
                if not destination.is_relative_to(target):
                    raise RuntimeError("component destination escaped staging")
                destination.parent.mkdir(parents=True, exist_ok=True)
                no_links(destination)
                source = paths[EXTRA[relative]].open("rb") if relative in EXTRA else archive.open(relative, "r")
                with source, destination.open("xb") as stream:
                    shutil.copyfileobj(source, stream, 8 * 1024 * 1024)
                    stream.flush()
                    os.fsync(stream.fileno())
                if sha(destination) != expected_hash:
                    raise RuntimeError(f"component readback mismatch: {relative}")
            result = {"state": "prepared", "created_utc": datetime.now(timezone.utc).isoformat(),
                      "aio_zip_sha256": ZIP_SHA, "member_count": len(expected),
                      "private_runtime_sources": {key: str(paths[key]) for key in ("nr", "sr")},
                      "note": "Local private staging only; do not include runtime files in diagnostics or distribution."}
            with (target / "stage-record.json").open("x", encoding="utf-8", newline="\n") as stream:
                json.dump(result, stream, ensure_ascii=False, indent=2)
                stream.write("\n")
        except Exception as exc:
            failed = {"state": "preparation_incomplete", "error": f"{type(exc).__name__}: {exc}"}
            with (target / "stage-failure.json").open("x", encoding="utf-8", newline="\n") as stream:
                json.dump(failed, stream, ensure_ascii=False, indent=2)
                stream.write("\n")
            raise
    print(f"private staging prepared: {target}")


if __name__ == "__main__":
    main()
