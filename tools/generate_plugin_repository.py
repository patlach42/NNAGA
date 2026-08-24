#!/usr/bin/env python3
"""Generate the checked-in LV2 plugin repository from trusted build inputs.

Each leaf ``*.lv2`` directory becomes one deterministic archive.  Metadata is
copied verbatim and binary references in TTL are resolved to the corresponding
``lib<basename>`` arm64 shared library, then copied into the bundle under the
exact basename named by TTL.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

ASSETS = Path("app/src/main/assets/lv2")
LIBS = Path("app/src/full/jniLibs/arm64-v8a")
METADATA = Path("app/src/main/assets/plugin_metadata.json")
OUTPUT = Path("plugin-repository")
VERSION = "1.0.0"
RELEASE = "2026-08-24"
BINARY_RE = re.compile(r"(?:lv2:binary|guiext:binary|ui:binary)\s+<([^>]+)>")
IDENTITY_ALIASES = {
    "GxVoodoFuzz": "GxVoodooFuzz",
    "gx_amp": "GxAmplifier-X",
    "gx_amp_stereo": "GxAmplifier-Stereo-X",
    "gx_chorus": "GxChorus-Stereo",
    "gx_delay": "GxDelay-Stereo",
    "gx_echo": "GxEcho-Stereo",
    "gx_reverb": "GxReverb-Stereo",
    "gx_zita_rev1": "GxZita_rev1-Stereo",
    "gxtape_st": "GxTapeStereo",
    "gxts9": "GxTubeScreamer",
    "aidadsp": "AIDA-X (headless)",
}


def package_id(stem: str) -> str:
    return "lv2." + re.sub(r"[^a-z0-9]", "", stem.lower())


def leaves() -> list[Path]:
    return sorted(
        (p for p in ASSETS.rglob("*.lv2") if p.is_dir() and (p / "manifest.ttl").is_file() and not any(c.is_dir() and c.name.endswith(".lv2") for c in p.iterdir())),
        key=lambda p: p.relative_to(ASSETS).as_posix(),
    )


def references(bundle: Path) -> list[str]:
    found: set[str] = set()
    for ttl in sorted(bundle.rglob("*.ttl")):
        for ref in BINARY_RE.findall(ttl.read_text(encoding="utf-8", errors="strict")):
            name = PurePosixPath(ref).name
            if name != ref or not name.endswith(".so"):
                raise ValueError(f"unsafe binary reference {ref!r} in {ttl}")
            found.add(name)
    return sorted(found)


def payload_bytes(bundle: Path, refs: list[str]) -> tuple[dict[str, bytes], list[str]]:
    files: dict[str, bytes] = {}
    for source in sorted(bundle.rglob("*")):
        if source.is_file():
            rel = source.relative_to(bundle).as_posix()
            files[rel] = source.read_bytes()
    missing: list[str] = []
    for ref in refs:
        source = LIBS / ("lib" + ref)
        if not source.is_file():
            missing.append(f"{bundle}: {ref} (expected {source})")
            continue
        files[ref] = source.read_bytes()
    return files, missing


def archive_bytes(bundle_name: str, files: dict[str, bytes]) -> bytes:
    with tempfile.SpooledTemporaryFile() as out:
        with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
            for rel in sorted(files):
                info = zipfile.ZipInfo(f"{bundle_name}/{rel}", date_time=(2020, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                zf.writestr(info, files[rel])
        out.seek(0)
        return out.read()

def canonical_name(value: str) -> str:
    return re.sub(r"[^a-z0-9]", "", value.casefold())
def metadata_for(name: str) -> tuple[str, list[str]]:
    data = json.loads(METADATA.read_text(encoding="utf-8"))
    def canonical_map(values: dict) -> dict[str, str]:
        out: dict[str, str] = {}
        for raw_key, raw_value in values.items():
            key, value = canonical_name(str(raw_key)), str(raw_value).strip()
            if key in out and out[key] != value:
                raise ValueError(f"conflicting metadata keys for {raw_key!r}")
            out[key] = value
        return out
    authors = canonical_map(data.get("authors", {}))
    categories = canonical_map(data.get("categories", {}))
    aliases: dict[str, str] = {}
    for stem, target in IDENTITY_ALIASES.items():
        key = canonical_name(stem)
        if key in aliases and aliases[key] != target:
            raise ValueError(f"conflicting identity aliases for {stem!r}")
        aliases[key] = target
    key = canonical_name(name)
    key = canonical_name(aliases.get(key, name))
    manufacturer = authors.get(key, "") or "Unknown"
    raw_category = categories.get(key, "")
    category = raw_category[:-6] if raw_category.endswith("Plugin") else raw_category
    if not category and raw_category == "Plugin":
        category = "Other"
    return manufacturer, [category] if category else []


def toml_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def manifest(package: str, name: str, archive: bytes) -> str:
    digest = hashlib.sha256(archive).hexdigest()
    manufacturer, tags = metadata_for(name)
    tags_text = "[" + ",".join(toml_string(tag) for tag in tags) + "]"
    return (f'schema = 1\nid = "{package}"\nname = {toml_string(name)}\nversion = "{VERSION}"\n'
            f'format = "lv2"\narch = ["arm64-v8a"]\nmanufacturer = {toml_string(manufacturer)}\n'
            f'tags = {tags_text}\ndescription = {toml_string(f"Bundled LV2 plugin {name}.")}\n'
            f'license = "GPL-3.0-or-later"\n\n'
            f'[payload]\nkind = "archive"\nurl = "../../payload/{package}/{VERSION}.zip"\n'
            f'sha256 = "{digest}"\nsize = {len(archive)}\n\n'
            f'[install]\nentry = {toml_string(name + ".lv2/manifest.ttl")}\n')


def atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)




def generate(check: bool = False) -> int:
    if not ASSETS.is_dir() or not LIBS.is_dir():
        raise SystemExit(f"missing input directory: {ASSETS} or {LIBS}")
    records: list[tuple[str, str, bytes, str]] = []
    missing: list[str] = []
    seen: set[str] = set()
    for bundle in leaves():
        stem = bundle.name[:-4]
        package = package_id(stem)
        if package in seen:
            raise SystemExit(f"duplicate package id: {package}")
        seen.add(package)
        refs = references(bundle)
        files, absent = payload_bytes(bundle, refs)
        missing.extend(absent)
        if absent:
            continue
        archive = archive_bytes(bundle.name, files)
        records.append((package, stem, archive, manifest(package, stem, archive)))
    if check:
        errors: list[str] = []
        expected_packages = {package for package, *_ in records}
        for kind in ("packages", "payload"):
            directory = OUTPUT / kind
            actual = {p.name for p in directory.iterdir()} if directory.is_dir() else set()
            extras = sorted(actual - expected_packages)
            missing_dirs = sorted(expected_packages - actual)
            errors.extend(f"{kind}/{name}" for name in extras + missing_dirs)
        for package, stem, archive, text in records:
            mp = OUTPUT / "packages" / package / "manifest.toml"
            zp = OUTPUT / "payload" / package / f"{VERSION}.zip"
            for directory, expected in ((mp.parent, {mp.name}), (zp.parent, {zp.name})):
                if directory.is_dir():
                    errors.extend(str(path) for path in sorted(directory.iterdir()) if path.name not in expected)
            if not mp.is_file() or mp.read_text() != text:
                errors.append(str(mp))
            if not zp.is_file() or zp.read_bytes() != archive:
                errors.append(str(zp))
        expected_index = index_text(records)
        if not (OUTPUT / "index.toml").is_file() or (OUTPUT / "index.toml").read_text() != expected_index:
            errors.append(str(OUTPUT / "index.toml"))
        if errors:
            raise SystemExit("repository out of date:\n" + "\n".join(errors))
    else:
        OUTPUT.mkdir(parents=True, exist_ok=True)
        expected_packages = {package for package, *_ in records}
        for kind in ("packages", "payload"):
            directory = OUTPUT / kind
            directory.mkdir(parents=True, exist_ok=True)
            for child in directory.iterdir():
                if child.is_dir() and child.name not in expected_packages:
                    shutil.rmtree(child)
                elif child.is_file():
                    child.unlink()
        for package in expected_packages:
            for directory, keep in (
                (OUTPUT / "packages" / package, "manifest.toml"),
                (OUTPUT / "payload" / package, f"{VERSION}.zip"),
            ):
                if directory.is_dir():
                    for child in directory.iterdir():
                        if child.name != keep:
                            if child.is_dir():
                                shutil.rmtree(child)
                            else:
                                child.unlink()
        for package, stem, archive, text in records:
            atomic_write(OUTPUT / "packages" / package / "manifest.toml", text.encode("utf-8"))
            atomic_write(OUTPUT / "payload" / package / f"{VERSION}.zip", archive)
        atomic_write(OUTPUT / "index.toml", index_text(records).encode("utf-8"))
    unknown = [stem for _, stem, _, _ in records if metadata_for(stem)[0] == "Unknown"]
    if unknown:
        print("Unknown plugin metadata:", flush=True)
        print("\n".join(sorted(unknown)), flush=True)
    if missing:
        print("Unsupported LV2 bundles (missing DSP/UI binaries):", flush=True)
        print("\n".join(sorted(missing)), flush=True)
        return 2
    print(f"Generated {len(records)} LV2 packages")
    return 0
def index_text(records: list[tuple[str, str, bytes, str]]) -> str:
    paths = ",".join(f'"packages/{package}/manifest.toml?v={RELEASE}"' for package, *_ in records)
    return f'schema = 1\nrepository = "nnaga-base"\nrelease = "{RELEASE}"\nmanifests = [{paths}]\n'


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify deterministic output without rewriting")
    raise SystemExit(generate(parser.parse_args().check))


if __name__ == "__main__":
    main()
