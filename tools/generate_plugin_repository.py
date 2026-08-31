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
import posixpath
import re
import shutil
import tempfile
import tomllib
import zipfile
from pathlib import Path, PurePosixPath
from urllib.parse import urlsplit

ASSETS = Path("app/src/main/assets/lv2")
LIBS = Path("app/src/full/jniLibs/arm64-v8a")
NATIVE_LIBS = Path("build/native_plugins/arm64-v8a")
METADATA = Path("app/src/main/assets/plugin_metadata.json")
DESCRIPTION_SOURCE = Path("plugin_descriptions.json")
NATIVE_METADATA = (
    Path("3rd_party/nnaga-native-plugin-sdk/package/filter.json"),
    Path("3rd_party/nnaga-native-plugin-sdk/package/shuffle.json"),
)
OUTPUT = Path(os.environ.get("NNAGA_PLUGIN_REPOSITORY", "../nnaga-plugin-repository"))
VERSION = "1.0.0"
RELEASE = "2026-09-01"
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
DISPLAY_NAMES = {
    "four_k_eq_2": "4K EQ 2",
}
LICENSES = {
    "fil4": "GPL-2.0-only",
    "4keq2": "GPL-3.0-or-later",
}
EXTRA_TAGS = {
    "fil4": ["Filter"],
    "4keq2": ["Filter"],
    "doubletracker": ["Delay", "Stereo", "Utility"],
}
DEFAULT_LV2_SOURCE = "https://github.com/djshaji/GxPlugins.lv2.Android"
LV2_SOURCES = {
    "lv2.aidadsp": "https://github.com/AidaDSP/aidadsp-lv2",
    "lv2.aidax": "https://github.com/AidaDSP/AIDA-X",
    "lv2.collisiondrive": "https://github.com/brummer10/CollisionDrive",
    "lv2.doubletracker": "https://github.com/Varcain/doubletracker.lv2",
    "lv2.fil4": "https://github.com/x42/fil4.lv2",
    "lv2.fourkeq2": "https://github.com/dusk-audio/dusk-audio-plugins",
    "lv2.gxcabsim": "https://github.com/brummer10/GxCabSim.lv2",
    "lv2.impulseloader": "https://github.com/brummer10/ImpulseLoader",
    "lv2.metaltone": "https://github.com/brummer10/MetalTone",
    "lv2.neuralampmodeler": "https://github.com/mikeoliphant/neural-amp-modeler-lv2",
    "lv2.neuralrack": "https://github.com/brummer10/NeuralRack",
    "lv2.xdarkterror": "https://github.com/brummer10/XDarkTerror.lv2",
    "lv2.xtinyterror": "https://github.com/brummer10/XTinyTerror.lv2",
}
MANUFACTURERS = {
    "lv2.fil4": "x42",
    "lv2.fourkeq2": "Dusk Audio",
}
LV2_TAG_OVERRIDES = {
    "lv2.fil4": ["EQ", "Filter"],
    "lv2.fourkeq2": ["EQ", "Filter"],
    "lv2.gxfz1s": ["Distortion", "Saturation"],
    "lv2.gxsloopyblue": ["Distortion", "Saturation"],
    "lv2.gxtimray": ["Distortion", "Saturation"],
    "lv2.gxaclipper": ["Distortion", "Dynamics"],
    "lv2.gxamp": ["Amplifier", "Cabinet"],
    "lv2.gxampstereo": ["Amplifier", "Cabinet", "Stereo"],
    "lv2.gxbmp": ["Distortion", "Saturation"],
    "lv2.gxcabinet": ["Cabinet", "Filter"],
    "lv2.gxcolwah": ["Filter", "Modulation"],
    "lv2.gxcompressor": ["Compressor", "Dynamics"],
    "lv2.gxdetune": ["Pitch", "Modulation"],
    "lv2.gxdigitaldelayst": ["Delay", "Stereo"],
    "lv2.gxfuzzface": ["Distortion", "Saturation"],
    "lv2.gxfuzzfacefm": ["Distortion", "Saturation"],
    "lv2.gxgcb95": ["Filter", "Modulation"],
    "lv2.gxjcm800pre": ["Amplifier", "Distortion"],
    "lv2.gxjcm800prest": ["Amplifier", "Distortion", "Stereo"],
    "lv2.gxmbcompressor": ["Compressor", "Dynamics"],
    "lv2.gxmbdelay": ["Delay", "Filter"],
    "lv2.gxmbdistortion": ["Distortion", "Saturation"],
    "lv2.gxmbecho": ["Delay", "Filter"],
    "lv2.gxmbreverb": ["Reverb", "Filter"],
    "lv2.gxshimmizita": ["Reverb", "Pitch"],
    "lv2.gxsusta": ["Distortion", "Saturation"],
    "lv2.gxswitchedtremolo": ["Modulation"],
    "lv2.gxtremolo": ["Modulation"],
    "lv2.gxvibe": ["Modulation", "Phaser"],
    "lv2.gxmetalamp": ["Amplifier", "Cabinet", "Distortion"],
    "lv2.gxmetalhead": ["Amplifier", "Distortion"],
    "lv2.gxtape": ["Filter", "Saturation"],
    "lv2.gxtapest": ["Filter", "Saturation", "Stereo"],
    "lv2.gxtubedelay": ["Delay", "Saturation"],
    "lv2.gxtubetremelo": ["Modulation", "Saturation"],
    "lv2.xdarkterror": ["Amplifier", "Distortion"],
    "lv2.xtinyterror": ["Amplifier", "Distortion"],
}


def license_for(name: str) -> str:
    return LICENSES.get(canonical_name(name), "GPL-3.0-or-later")


def package_id(stem: str) -> str:
    return "lv2." + re.sub(r"[^a-z0-9]", "", stem.lower())




_FALLBACK_DESCRIPTIONS = {
    "GxBoobTube": "Tube boost with gentle overdrive and compression.",
    "GxKnightFuzz": "Dark, high-harmonic fuzz distortion.",
    "GxSuperFuzz": "Harmonic-rich SuperFuzz-style distortion.",
    "GxVoodoFuzz": "Fuzz, tone shaping, and boost.",
    "gx_alembic": "Alembic-style tube preamp and tone stack.",
    "gx_duck_delay": "Envelope-controlled ducking delay.",
    "gx_duck_delay_st": "Stereo envelope-controlled ducking delay.",
    "gx_studiopre": "Alembic-style tube studio preamp.",
    "gx_studiopre_st": "Stereo Alembic-style tube studio preamp.",
    "gx_w20": "W20-style tube preamp and tone stack.",
}


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


_GENERIC_DESCRIPTIONS = {
    "amplifier simulation",
    "amp/cabinet simulator",
    "chorus effect",
    "delay effect",
    "distortion/overdrive effect",
    "dynamic range compressor",
    "equalizer",
    "expander/gate",
    "filter/tone shaping effect",
    "flanger effect",
    "modulation effect",
    "noise gate",
    "overdrive/distortion",
    "overdrive pedal simulation",
    "phaser effect",
    "pitch shifting effect",
    "reverb effect",
    "signal analyser",
    "tube amp simulation",
    "valve amplifier simulation",
}
_BOILERPLATE_RE = re.compile(
    r"\b(?:LV2\s+)?plugin(?:s)?\b|"
    r"\b(?:archive|package|install(?:ation)?|bundle(?:d)?)\b",
    re.IGNORECASE,
)


def normalize_description(value: str) -> str:
    text = re.sub(r"\s+", " ", str(value)).strip()
    text = _BOILERPLATE_RE.sub("", text)
    text = re.sub(r"\s+([,.;:!?])", r"\1", text)
    text = re.sub(r"([,.;:!?]){2,}", r"\1", text)
    text = re.sub(r"\s{2,}", " ", text).strip(" ,;:-")
    if text and text[-1] not in ".!?":
        text += "."
    return text


def _fallback_description(name: str, category: str) -> str:
    if name in _FALLBACK_DESCRIPTIONS:
        return _FALLBACK_DESCRIPTIONS[name]
    kind = {
        "Amplifier": "amplifier and cabinet tones",
        "Analyser": "audio signal analysis",
        "Chorus": "stereo chorus modulation",
        "Compressor": "dynamic range control",
        "Delay": "echo and delay effects",
        "Distortion": "distortion and overdrive tones",
        "EQ": "equalization and tone shaping",
        "Envelope": "envelope-controlled wah effects",
        "Expander": "expansion and noise-gate control",
        "Filter": "filtering and tone shaping",
        "Flanger": "flanger modulation",
        "Gate": "noise-gate control",
        "Modulator": "modulation effects",
        "Phaser": "phaser modulation",
        "Pitch": "pitch shifting",
        "Reverb": "room and reverb effects",
        "Simulator": "guitar amplifier and cabinet modeling",
        "Utility": "utility tone and signal processing",
        "Mixer": "mixing and signal routing",
        "Plugin": "audio processing",
    }.get(category, "audio processing")
    return f"{name} provides {kind} for guitar and music production."


def metadata_for(name: str, package: str) -> tuple[str, list[str], str, str]:
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
    sources = canonical_map(data.get("sources", {}))
    description_data = json.loads(DESCRIPTION_SOURCE.read_text(encoding="utf-8"))
    descriptions = canonical_map(description_data.get("descriptions", description_data))
    aliases: dict[str, str] = {}
    for stem, target in IDENTITY_ALIASES.items():
        key = canonical_name(stem)
        if key in aliases and aliases[key] != target:
            raise ValueError(f"conflicting identity aliases for {stem!r}")
        aliases[key] = target
    key = canonical_name(name)
    key = canonical_name(aliases.get(key, name))
    manufacturer = MANUFACTURERS.get(package, authors.get(key, "")).strip() or "Unknown"
    raw_category = categories.get(key, "")
    category = raw_category[:-6] if raw_category.endswith("Plugin") else raw_category
    description = normalize_description(descriptions.get(key, ""))
    if (
        not description
        or description.casefold() in _GENERIC_DESCRIPTIONS
        or "music production" in description.casefold()
        or len(description) > 180
    ):
        description = _fallback_description(name, category)
    if not 30 <= len(description) <= 180:
        raise ValueError(f"description must be 30-180 characters: {name}")
    source = sources.get(key) or LV2_SOURCES.get(package, DEFAULT_LV2_SOURCE)
    parsed_source = urlsplit(source)
    unsafe_source = (
        parsed_source.scheme != "https"
        or not parsed_source.hostname
        or parsed_source.username
        or parsed_source.password
        or parsed_source.query
        or parsed_source.fragment
        or not parsed_source.path
        or parsed_source.path.lower().endswith(".git")
        or "%2e" in parsed_source.path.lower()
        or posixpath.normpath(parsed_source.path) != parsed_source.path
    )
    if unsafe_source:
        raise ValueError(f"invalid plugin source URL: {source}")
    tags = list(LV2_TAG_OVERRIDES.get(package, [category] if category else []))
    if package not in LV2_TAG_OVERRIDES:
        tags.extend(tag for tag in EXTRA_TAGS.get(key, []) if tag not in tags)
    return manufacturer, tags, description, source


def toml_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def manifest(package: str, name: str, bundle_stem: str, archive: bytes) -> str:
    digest = hashlib.sha256(archive).hexdigest()
    version = VERSION
    manufacturer, tags, description, source = metadata_for(name, package)
    tags_text = "[" + ",".join(toml_string(tag) for tag in tags) + "]"
    license_name = license_for(name)
    return (f'schema = 1\nid = "{package}"\nname = {toml_string(name)}\nversion = "{version}"\n'
            f'format = "lv2"\nsource = {toml_string(source)}\narch = ["arm64-v8a"]\n'
            f'manufacturer = {toml_string(manufacturer)}\ntags = {tags_text}\n'
            f'description = {toml_string(description)}\nlicense = {toml_string(license_name)}\n\n'
            f'[payload]\nkind = "archive"\nurl = "../../payload/{package}/{version}.zip"\n'
            f'sha256 = "{digest}"\nsize = {len(archive)}\n\n'
            f'[install]\nentry = {toml_string(bundle_stem + ".lv2/manifest.ttl")}\n')




def native_record(metadata_path: Path) -> tuple[str, str, bytes, str]:
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    package = metadata["repository_id"]
    library = metadata["library"]
    binary = NATIVE_LIBS / library
    if not binary.is_file():
        raise SystemExit(f"missing native plugin binary: {binary}")
    archive = archive_bytes("arm64-v8a", {library: binary.read_bytes()})
    digest = hashlib.sha256(archive).hexdigest()
    manifest_text = (
        f'schema = 1\nid = {toml_string(package)}\nname = {toml_string(metadata["name"])}\n'
        f'version = {toml_string(metadata["version"])}\nformat = "native"\n'
        f'source = {toml_string(metadata["source"])}\narch = ["arm64-v8a"]\n'
        f'manufacturer = {toml_string(metadata["manufacturer"])}\n'
        f'tags = {json.dumps(metadata["tags"], ensure_ascii=False, separators=(",", ":"))}\n'
        f'description = {toml_string(metadata["description"])}\nlicense = {toml_string(metadata["license"])}\n\n'
        f'[payload]\nkind = "archive"\nurl = "../../payload/{package}/{metadata["version"]}.zip"\n'
        f'sha256 = "{digest}"\nsize = {len(archive)}\n\n'
        f'[install]\nentry = "arm64-v8a/{library}"\n'
    )
    return package, metadata["name"], archive, manifest_text
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




def generate(check: bool = False, output: Path | None = None) -> int:
    global OUTPUT
    if output is not None:
        OUTPUT = output
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
        display_name = DISPLAY_NAMES.get(stem, stem)
        records.append((package, display_name, archive,
                        manifest(package, display_name, stem, archive)))
    for native_record_value in (native_record(path) for path in NATIVE_METADATA):
        if native_record_value[0] in seen:
            raise SystemExit(f"duplicate package id: {native_record_value[0]}")
        records.append(native_record_value)
    record_versions = {
        package: str(tomllib.loads(text)["version"])
        for package, _, _, text in records
    }
    if check:
        errors: list[str] = []
        expected_packages = {package for package, *_ in records}
        for kind in ("packages", "payload"):
            directory = OUTPUT / kind
            actual = {p.name for p in directory.iterdir() if p.name.startswith(("lv2.", "native."))} if directory.is_dir() else set()
            extras = sorted(actual - expected_packages)
            missing_dirs = sorted(expected_packages - actual)
            errors.extend(f"{kind}/{name}" for name in extras + missing_dirs)
        for package, stem, archive, text in records:
            mp = OUTPUT / "packages" / package / "manifest.toml"
            zp = OUTPUT / "payload" / package / f"{record_versions[package]}.zip"
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
                if child.is_dir() and child.name.startswith(("lv2.", "native.")) and child.name not in expected_packages:
                    shutil.rmtree(child)
        for package in expected_packages:
            for directory, keep in (
                (OUTPUT / "packages" / package, "manifest.toml"),
                (OUTPUT / "payload" / package, f"{record_versions[package]}.zip"),
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
            atomic_write(OUTPUT / "payload" / package / f"{record_versions[package]}.zip", archive)
        atomic_write(OUTPUT / "index.toml", index_text(records).encode("utf-8"))
    unknown = [stem for package, stem, _, _ in records if package.startswith("lv2.") and metadata_for(stem, package)[0] == "Unknown"]
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
    generated = {package: text for package, _, _, text in records}
    manifests = sorted((OUTPUT / "packages").glob("*/manifest.toml"))
    rows: list[str] = []
    for path in manifests:
        package = path.parent.name
        text = generated.get(package, path.read_text(encoding="utf-8"))
        data = tomllib.loads(text)
        tags = json.dumps(data.get("tags", []), ensure_ascii=False, separators=(",", ":"))
        q = lambda value: json.dumps(str(value), ensure_ascii=False)
        rows.append(
            "[[packages]]\n"
            f"manifest = {q(path.relative_to(OUTPUT).as_posix() + '?v=' + RELEASE)}\n"
            f"id = {q(data['id'])}\nname = {q(data['name'])}\n"
            f"version = {q(data['version'])}\nformat = {q(data['format'])}\n"
            f"description = {q(data['description'])}\n"
            f"manufacturer = {q(data['manufacturer'])}\n"
            f"source = {q(data['source'])}\n"
            f"tags = {tags}\n"
        )
    return f"schema = 2\nrepository = \"nnaga-plugin-repository\"\nrelease = \"{RELEASE}\"\n\n" + "\n".join(rows)
def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify deterministic output without rewriting")
    parser.add_argument("--output", type=Path, help="standalone repository output directory")
    args = parser.parse_args()
    raise SystemExit(generate(args.check, args.output))


if __name__ == "__main__":
    main()
