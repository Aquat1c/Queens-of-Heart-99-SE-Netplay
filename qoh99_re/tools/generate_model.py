#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import tomllib
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = REPO / "model" / "qoh99_manifest.toml"
GENERATOR_REL = Path("tools/generate_model.py")
LOCK_REL = Path("generated/model/manifest.lock.json")

OUTPUT_RELS = (
    Path("include/qoh99/generated/AddressConstants.hpp"),
    Path("include/qoh99/generated/FunctionCatalog.hpp"),
    Path("include/qoh99/generated/BuildProfiles.hpp"),
    Path("include/qoh99/generated/GameplayRegions.hpp"),
    Path("include/qoh99/generated/NativeSymbolCatalog.hpp"),
)

IMAGE_BASE = 0x400000
IDENTITY_LAST_ENTRY_VA = 0x472B80
SHIFTED_FIRST_ENTRY_NOVEMBER_VA = 0x4736C0
ENTRY_SHIFT = 0x20
CODE_DATA_BOUNDARY_VA = 0x499000
ENTRY_WINDOW_BYTES = 16

NATIVE_SYMBOL_KINDS = {
    "FunctionEntry", "InteriorCode", "DataGlobal", "CodePointerStorage",
    "DataTableWord",
}
NATIVE_SHIFT_CLASSES = {
    "IdentityLayout", "ShiftedEntry", "DataUnchanged",
    "InteriorOfIdentityFunction", "SemanticallyEdited",
}
NATIVE_RUNTIME_USES = {
    "NotCallable", "SeamResolved", "SeamResolvedVerifyOnly", "DataAccess",
    "RawLiteralHookTarget", "RawLiteralCallTarget", "RawLiteralSiteTag",
    "RawLiteralRangeBound", "RawLiteralReturnKey",
}
VALUE_BEARING_KINDS = {"CodePointerStorage", "DataTableWord"}
CODE_KINDS = {"FunctionEntry", "InteriorCode"}

def validate_prologue_opt_in(row: dict[str, Any], label: str) -> None:
    if "prologue_bytes" not in row:
        return
    _require(row.get("kind") in CODE_KINDS,
             f"{label}: prologue_bytes is a CODE-row opt-in; a data row has no "
             "prologue to curate")
    _require(row["prologue_bytes"] == ENTRY_WINDOW_BYTES,
             f"{label}: prologue_bytes must be exactly {ENTRY_WINDOW_BYTES} "
             f"(kNativeEntryWindowBytesCompared). If this entry is shorter than "
             f"{ENTRY_WINDOW_BYTES} bytes, OMIT prologue_bytes rather than "
             "shortening it -- a second window length would mean two different "
             "claims wearing one name.")

RUNTIME_QUALIFIED_BUILDS = {"se_2006", "game_2000"}

CHR_HEADER_REVISION_VA = 0x4A71E0
CHR_HEADER_REVISION_SYMBOL = "dataTableWord4A71E0"

SIMULATION_PROBE_PATH = "yosie/yosie.chr"

CONTENT_PRECONDITION_FIELDS = (
    "chr_header_revision_va", "chr_header_revision",
    "simulation_probe_path", "simulation_probe_sha256",
)

class ManifestError(ValueError):
    pass

def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest().upper()

def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()

def load_manifest(path: Path = DEFAULT_MANIFEST) -> tuple[dict[str, Any], bytes]:
    raw = path.read_bytes()
    try:
        model = tomllib.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, tomllib.TOMLDecodeError) as exc:
        raise ManifestError(f"cannot parse {path}: {exc}") from exc
    return model, raw

def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ManifestError(message)

def _require_unique(rows: list[dict[str, Any]], field: str, section: str) -> None:
    values = [row.get(field) for row in rows]
    _require(all(value is not None for value in values),
             f"{section}: every row needs {field}")
    duplicates = sorted({value for value in values if values.count(value) > 1})
    _require(not duplicates, f"{section}: duplicate {field}: {duplicates}")

def native_symbol_enum_name(key: str) -> str:
    return key[:1].upper() + key[1:]

def derive_december_va(november_va: int, shift_class: str, label: str) -> int:
    if shift_class == "IdentityLayout":
        _require(november_va <= IDENTITY_LAST_ENTRY_VA,
                 f"{label}: IdentityLayout requires november_va <= "
                 f"0x{IDENTITY_LAST_ENTRY_VA:X}")
        return november_va
    if shift_class == "ShiftedEntry":
        _require(november_va >= SHIFTED_FIRST_ENTRY_NOVEMBER_VA,
                 f"{label}: ShiftedEntry requires november_va >= "
                 f"0x{SHIFTED_FIRST_ENTRY_NOVEMBER_VA:X}")
        return november_va + ENTRY_SHIFT
    if shift_class == "DataUnchanged":
        _require(november_va >= CODE_DATA_BOUNDARY_VA,
                 f"{label}: DataUnchanged requires november_va >= "
                 f"0x{CODE_DATA_BOUNDARY_VA:X} (static data never moves; a code "
                 "address must never be classified DataUnchanged)")
        return november_va
    if shift_class == "InteriorOfIdentityFunction":
        _require(november_va <= IDENTITY_LAST_ENTRY_VA,
                 f"{label}: InteriorOfIdentityFunction requires november_va <= "
                 f"0x{IDENTITY_LAST_ENTRY_VA:X}")
        return november_va
    raise ManifestError(
        f"{label}: shift_class '{shift_class}' is not derivable; the December "
        "column must be stated explicitly and carry a note")

def validate_functions_layout(functions: list[dict[str, Any]]) -> None:
    for row in functions:
        label = row.get("cxx")
        november = row["se_2006_va"]
        december = row["game_2000_va"]
        if november <= IDENTITY_LAST_ENTRY_VA:
            expected_class, expected_mapping = "IdentityLayout", "IdentityLayout"
        elif november >= SHIFTED_FIRST_ENTRY_NOVEMBER_VA:
            expected_class = "ShiftedEntry"
            expected_mapping = "ExplicitProfileEntry"
        else:
            raise ManifestError(
                f"{label}: november_va 0x{november:X} falls in the layout gap "
                f"0x{IDENTITY_LAST_ENTRY_VA:X}..0x"
                f"{SHIFTED_FIRST_ENTRY_NOVEMBER_VA:X} (the body of the edited "
                "sub_472B80); no function entry can live there")
        derived = derive_december_va(november, expected_class, label)
        _require(december == derived,
                 f"{label}: december column 0x{december:X} does not match the "
                 f"layout rule ({expected_class} of 0x{november:X} is "
                 f"0x{derived:X})")
        _require(row["evidence_va"] == december,
                 f"{label}: evidence_va must equal the December column")
        _require(row["runtime_mapping"] == expected_mapping,
                 f"{label}: runtime_mapping must be {expected_mapping}")
        _require(row.get("kind") == "FunctionEntry",
                 f"{label}: function rows must be kind=FunctionEntry")
        _require(row.get("runtime_use") in NATIVE_RUNTIME_USES,
                 f"{label}: invalid runtime_use")
        validate_prologue_opt_in(row, label)

def validate_native_symbols(model: dict[str, Any]) -> None:
    rows = model["native_symbols"]
    _require_unique(rows, "key", "native_symbols")
    _require_unique(rows, "november_va", "native_symbols")
    for row in rows:
        label = row.get("key")
        _require(isinstance(label, str)
                 and re.fullmatch(r"[a-z][A-Za-z0-9]*", label) is not None,
                 f"native_symbols: invalid key {label!r}")
        kind = row.get("kind")
        shift_class = row.get("shift_class")
        _require(kind in NATIVE_SYMBOL_KINDS, f"{label}: invalid kind")
        _require(shift_class in NATIVE_SHIFT_CLASSES,
                 f"{label}: invalid shift_class")
        _require(row.get("runtime_use") in NATIVE_RUNTIME_USES,
                 f"{label}: invalid runtime_use")
        for field in ("november_va", "december_va"):
            _require(isinstance(row.get(field), int)
                     and IMAGE_BASE <= row[field] < 0x900000,
                     f"{label}: invalid {field}")
        note = row.get("note")
        _require(isinstance(note, str) and note.strip() != "",
                 f"{label}: every native symbol needs a note saying why it "
                 "exists")
        validate_prologue_opt_in(row, label)

        if shift_class == "SemanticallyEdited":
            _require(kind == "FunctionEntry",
                     f"{label}: SemanticallyEdited applies to function entries")
        else:
            derived = derive_december_va(row["november_va"], shift_class, label)
            _require(row["december_va"] == derived,
                     f"{label}: december_va 0x{row['december_va']:X} does not "
                     f"match the layout rule ({shift_class} of "
                     f"0x{row['november_va']:X} is 0x{derived:X})")

        if kind == "InteriorCode":
            _require(shift_class == "InteriorOfIdentityFunction",
                     f"{label}: interior code must be classified "
                     "InteriorOfIdentityFunction so nothing shift-derives it")
        if kind in CODE_KINDS:
            _require(row["november_va"] < CODE_DATA_BOUNDARY_VA,
                     f"{label}: code symbol must live below the .rdata "
                     f"boundary 0x{CODE_DATA_BOUNDARY_VA:X}")
        else:
            _require(row["november_va"] >= CODE_DATA_BOUNDARY_VA,
                     f"{label}: data symbol must live at or above the .rdata "
                     f"boundary 0x{CODE_DATA_BOUNDARY_VA:X}")
            _require(shift_class == "DataUnchanged",
                     f"{label}: a {kind} row must be DataUnchanged; static "
                     "data VAs are identical in both builds and the entry "
                     "shift must never be applied to them")

        has_values = ("november_value" in row) or ("december_value" in row)
        if kind in VALUE_BEARING_KINDS:
            _require(has_values,
                     f"{label}: {kind} rows carry a per-build VALUE; an "
                     "address-only column cannot express them")
            for field in ("november_value", "december_value"):
                _require(isinstance(row.get(field), int)
                         and 0 <= row[field] <= 0xFFFFFFFF,
                         f"{label}: invalid {field}")
        else:
            _require(not has_values,
                     f"{label}: only {sorted(VALUE_BEARING_KINDS)} rows may "
                     "carry per-build values")

    names: dict[str, str] = {}
    for source, keys in (
        ("functions", [row["cxx"][1:] for row in model["functions"]]),
        ("data_symbols", [row["cxx"][1:] for row in model["data_symbols"]]),
        ("native_symbols",
         [native_symbol_enum_name(row["key"]) for row in rows]),
    ):
        for name in keys:
            _require(name not in names,
                     f"native symbol id '{name}' is declared by both "
                     f"{names.get(name)} and {source}")
            names[name] = source

    addresses: dict[int, str] = {}
    for source, pairs in (
        ("functions", [(row["se_2006_va"], row["cxx"]) for row in model["functions"]]),
        ("data_symbols", [(row["va"], row["cxx"]) for row in model["data_symbols"]]),
        ("native_symbols", [(row["november_va"], row["key"]) for row in rows]),
    ):
        for va, key in pairs:
            _require(va not in addresses,
                     f"November VA 0x{va:X} is claimed twice: "
                     f"{addresses.get(va)} and {source}:{key}")
            addresses[va] = f"{source}:{key}"

def validate_build_content_preconditions(
    builds: list[dict[str, Any]], native_symbols: list[dict[str, Any]]
) -> None:
    revision_symbol = next(
        (row for row in native_symbols
         if row["key"] == CHR_HEADER_REVISION_SYMBOL), None)
    _require(revision_symbol is not None,
             f"native_symbols: required {CHR_HEADER_REVISION_SYMBOL} is missing; "
             "the build content preconditions have nothing to bind to")
    _require(revision_symbol["november_va"] == CHR_HEADER_REVISION_VA
             and revision_symbol["december_va"] == CHR_HEADER_REVISION_VA,
             f"{CHR_HEADER_REVISION_SYMBOL}: must be the shared table word at "
             f"0x{CHR_HEADER_REVISION_VA:X}")
    revision_by_build = {
        "se_2006": revision_symbol["november_value"],
        "game_2000": revision_symbol["december_value"],
    }

    for row in builds:
        preconditions = row.get("content_preconditions")
        _require(isinstance(preconditions, dict),
                 f"{row['id']}: content_preconditions must be an inline table")
        _require(set(preconditions) == set(CONTENT_PRECONDITION_FIELDS),
                 f"{row['id']}: content_preconditions must contain exactly "
                 f"{sorted(CONTENT_PRECONDITION_FIELDS)}")
        _require(preconditions["chr_header_revision_va"]
                 == CHR_HEADER_REVISION_VA,
                 f"{row['id']}: chr_header_revision_va must be "
                 f"0x{CHR_HEADER_REVISION_VA:X}")
        _require(preconditions["chr_header_revision"]
                 == revision_by_build[row["id"]],
                 f"{row['id']}: chr_header_revision "
                 f"{preconditions['chr_header_revision']} disagrees with the "
                 f"{CHR_HEADER_REVISION_SYMBOL} value read from the image "
                 f"({revision_by_build[row['id']]})")

        path = preconditions["simulation_probe_path"]
        digest = preconditions["simulation_probe_sha256"]
        if row["id"] == "game_2000":
            _require(path == SIMULATION_PROBE_PATH,
                     f"{row['id']}: simulation_probe_path must be "
                     f"{SIMULATION_PROBE_PATH}")
            _require(isinstance(digest, str) and len(digest) == 64
                     and all(c in "0123456789ABCDEF" for c in digest),
                     f"{row['id']}: simulation_probe_sha256 must be 64 "
                     "upper-case hex characters")
        else:
            _require(path == "" and digest == "",
                     f"{row['id']}: only the December candidate carries a "
                     "simulation probe; November must state an empty path and "
                     "an empty digest")
        _require(path == path.lower().replace("\\", "/"),
                 f"{row['id']}: resource_filetime_path must be normalized to "
                 "lower-case '/' form")

def validate_manifest(model: dict[str, Any]) -> None:
    _require(model.get("schema_version") == 1, "schema_version must be 1")
    _require(model.get("generator_version") == 1, "generator_version must be 1")

    sections = {
        "data_symbols": model.get("data_symbols"),
        "sizes": model.get("sizes"),
        "functions": model.get("functions"),
        "native_symbols": model.get("native_symbols"),
        "regions": model.get("regions"),
        "pointer_children": model.get("pointer_children"),
        "builds": model.get("builds"),
    }
    for name, rows in sections.items():
        _require(isinstance(rows, list), f"{name} must be an array")

    expected = model.get("expected_counts")
    _require(isinstance(expected, dict), "expected_counts must be an inline table")
    for name, rows in sections.items():
        _require(expected.get(name) == len(rows),
                 f"{name}: expected {expected.get(name)}, found {len(rows)}")

    data = sections["data_symbols"]
    _require_unique(data, "cxx", "data_symbols")
    _require_unique(data, "name", "data_symbols")
    _require_unique(data, "va", "data_symbols")
    valid_mem_classes = {
        "ReadOnlyTable", "MutableGameplay", "RenderCosmetic",
        "UnsafeOSPointer", "WallClockPacing", "ControlFlow",
    }
    valid_confidence = {
        "RuntimeVerified", "StaticAnalysis", "StrongLikely", "Unknown",
    }
    canonical_cxx = {row["cxx"] for row in data}
    aliases: set[str] = set()
    for row in data:
        _require(re.fullmatch(r"k[A-Za-z0-9]+", row["cxx"]) is not None,
                 f"data symbol has invalid C++ name: {row['cxx']}")
        _require(isinstance(row.get("va"), int) and 0 < row["va"] <= 0xFFFFFFFF,
                 f"{row['cxx']}: invalid VA")
        _require(isinstance(row.get("size"), int) and row["size"] > 0,
                 f"{row['cxx']}: invalid size")
        _require(row["va"] + row["size"] <= 0x1_0000_0000,
                 f"{row['cxx']}: VA+size overflows GameAddr")
        _require(row.get("mem_class") in valid_mem_classes,
                 f"{row['cxx']}: invalid mem_class")
        _require(row.get("confidence") in valid_confidence,
                 f"{row['cxx']}: invalid confidence")
        for alias in row.get("legacy_aliases", []):
            _require(alias not in aliases and alias not in canonical_cxx,
                     f"{row['cxx']}: duplicate/colliding alias {alias}")
            aliases.add(alias)
    host = next((row for row in data if row["cxx"] == "kNetIsHost"), None)
    _require(host is not None, "data_symbols: required kNetIsHost is missing")
    _require(host["name"] == "netIsHost" and host["va"] == 0x66EE00
             and host["size"] == 4,
             "data_symbols: kNetIsHost must be netIsHost @ 0x66EE00 size 4")

    sizes = sections["sizes"]
    _require_unique(sizes, "cxx", "sizes")
    _require_unique(sizes, "name", "sizes")
    for row in sizes:
        _require(isinstance(row.get("value"), int) and row["value"] > 0,
                 f"{row.get('cxx')}: invalid size constant")

    functions = sections["functions"]
    _require_unique(functions, "cxx", "functions")
    _require_unique(functions, "name", "functions")
    _require_unique(functions, "evidence_va", "functions")
    for row in functions:
        for field in ("evidence_va", "game_2000_va", "se_2006_va"):
            _require(isinstance(row.get(field), int)
                     and 0x400000 <= row[field] < 0x900000,
                     f"{row.get('cxx')}: invalid {field}")
        _require(row.get("runtime_mapping") in
                 {"IdentityLayout", "ExplicitProfileEntry", "SignatureVerified"},
                 f"{row.get('cxx')}: invalid runtime_mapping")
    validate_functions_layout(functions)
    validate_native_symbols(model)

    builds = sections["builds"]
    _require_unique(builds, "id", "builds")
    _require({row["id"] for row in builds} == {"game_2000", "se_2006"},
             "builds must contain exactly game_2000 and se_2006")
    expected_runtime_profiles = {
        "game_2000": ("qoh99-december-candidate", "Qualified"),
        "se_2006": ("qoh99-se1126", "Qualified"),
    }
    for row in builds:
        for prefix in ("binary",):
            _require(isinstance(row.get(f"{prefix}_path"), str),
                     f"{row['id']}: missing {prefix}_path")
            digest = row.get(f"{prefix}_sha256")
            _require(isinstance(digest, str)
                     and re.fullmatch(r"[0-9A-F]{64}", digest) is not None,
                     f"{row['id']}: invalid {prefix}_sha256")
            _require(isinstance(row.get(f"{prefix}_size"), int)
                     and row[f"{prefix}_size"] > 0,
                     f"{row['id']}: invalid {prefix}_size")
        _require(isinstance(row.get("coff_timestamp"), int),
                 f"{row['id']}: invalid coff_timestamp")
        column = row.get("function_address_column")
        _require(column in {"game_2000_va", "se_2006_va"},
                 f"{row['id']}: invalid function_address_column")
        expected_profile = expected_runtime_profiles[row["id"]]
        _require(row.get("runtime_profile_id") == expected_profile[0],
                 f"{row['id']}: invalid runtime_profile_id")
        _require(row.get("runtime_qualification") == expected_profile[1],
                 f"{row['id']}: invalid runtime_qualification")
        _require(row.get("simulation_content_manifest_policy")
                 == "requires-future-manifest-attestation",
                 f"{row['id']}: invalid simulation_content_manifest_policy")
        _require(row.get("executable_identity_establishes_simulation_content")
                 is False,
                 f"{row['id']}: executable identity must not establish "
                 "simulation content compatibility")
        _require((row["runtime_qualification"] == "Qualified")
                 == (row["id"] in RUNTIME_QUALIFIED_BUILDS),
                 f"{row['id']}: runtime_qualification disagrees with the "
                 "generator's pinned runnable-build set; qualifying a build is "
                 "a deliberate two-file change")
    _require(sum(1 for row in builds
                 if row["runtime_qualification"] == "Qualified")
             == len(RUNTIME_QUALIFIED_BUILDS),
             "the qualified-build count must equal the generator's pinned set")

    validate_build_content_preconditions(builds, sections["native_symbols"])

    regions = sections["regions"]
    _require_unique(regions, "name", "regions")
    valid_relevance = {
        "DeterministicGameplay", "RenderCosmetic", "WallClockPacing",
        "ExternalResource", "Unknown",
    }
    valid_semantics = {"Gameplay", "Render", "Timing", "External", "Unknown"}
    valid_safety = {
        "PlainBytes", "FixedImagePointers", "ProcessLocalPointers",
        "ReadOnly", "WallClock", "Unknown",
    }
    for row in regions:
        _require(row.get("relevance") in valid_relevance,
                 f"region {row.get('name')}: invalid relevance")
        _require(row.get("semantic") in valid_semantics,
                 f"region {row.get('name')}: invalid semantic")
        _require(row.get("safety") in valid_safety,
                 f"region {row.get('name')}: invalid safety")
        _require(row.get("confidence") in valid_confidence,
                 f"region {row.get('name')}: invalid confidence")
        _require(isinstance(row.get("address"), int) and row["address"] > 0,
                 f"region {row.get('name')}: invalid address")
        _require(isinstance(row.get("size"), int) and row["size"] > 0,
                 f"region {row.get('name')}: invalid size")
        for field in ("capture", "restore", "gameplay_hash", "render_hash"):
            _require(isinstance(row.get(field), bool),
                     f"region {row.get('name')}: {field} must be bool")
        _require(not row["restore"] or row["capture"],
                 f"region {row['name']}: restore requires capture")
        _require(not row["gameplay_hash"] or row["capture"],
                 f"region {row['name']}: gameplay_hash requires capture")
        _require(not row["render_hash"] or row["capture"],
                 f"region {row['name']}: render_hash requires capture")
        if row["safety"] in {"ProcessLocalPointers", "ReadOnly", "WallClock", "Unknown"}:
            _require(not any(row[field] for field in
                             ("capture", "restore", "gameplay_hash", "render_hash")),
                     f"region {row['name']}: unsafe/read-only region has active policy")

    children = sections["pointer_children"]
    _require_unique(children, "name", "pointer_children")
    for row in children:
        _require(isinstance(row.get("pointer_address"), int)
                 and row["pointer_address"] > 0,
                 f"pointer child {row.get('name')}: invalid pointer_address")
        _require(isinstance(row.get("pointee_size"), int)
                 and row["pointee_size"] > 0,
                 f"pointer child {row.get('name')}: invalid pointee_size")
        _require(isinstance(row.get("nullable"), bool),
                 f"pointer child {row.get('name')}: nullable must be bool")
        _require(row.get("hash_identity") in
                 {"OmitRawAddress", "StableImage", "OwnerRelative"},
                 f"pointer child {row.get('name')}: invalid hash_identity")

def _coff_timestamp(path: Path) -> int:
    data = path.read_bytes()
    _require(len(data) >= 0x40, f"{path}: file too small for DOS header")
    pe_offset = int.from_bytes(data[0x3C:0x40], "little")
    _require(pe_offset + 12 <= len(data), f"{path}: invalid PE offset")
    _require(data[pe_offset:pe_offset + 4] == b"PE\0\0",
             f"{path}: missing PE signature")
    return int.from_bytes(data[pe_offset + 8:pe_offset + 12], "little")

def evidence_root() -> Path | None:
    configured = os.environ.get("QOH99_RE_EVIDENCE_ROOT")
    return Path(configured).resolve() if configured else None

def verify_build_artifacts(model: dict[str, Any], root: Path) -> None:
    for build in model["builds"]:
        path = root / build["binary_path"]
        _require(path.is_file(), f"{build['id']}: artifact missing: {path}")
        actual_size = path.stat().st_size
        _require(actual_size == build["binary_size"],
                 f"{build['id']}: binary size {actual_size} != "
                 f"{build['binary_size']}")
        actual_hash = sha256_file(path)
        _require(actual_hash == build["binary_sha256"],
                 f"{build['id']}: binary SHA-256 {actual_hash} != "
                 f"{build['binary_sha256']}")
        actual_stamp = _coff_timestamp(path)
        _require(actual_stamp == build["coff_timestamp"],
                 f"{build['id']}: COFF TimeDateStamp 0x{actual_stamp:08X} != "
                 f"0x{build['coff_timestamp']:08X}")

def _pe_sections(data: bytes, label: str) -> tuple[int, list[tuple[str, int, int, int, int]]]:
    _require(len(data) >= 0x40, f"{label}: file too small for DOS header")
    pe = int.from_bytes(data[0x3C:0x40], "little")
    _require(data[pe:pe + 4] == b"PE\0\0", f"{label}: missing PE signature")
    section_count = int.from_bytes(data[pe + 6:pe + 8], "little")
    optional_size = int.from_bytes(data[pe + 20:pe + 22], "little")
    image_base = int.from_bytes(data[pe + 24 + 28:pe + 24 + 32], "little")
    offset = pe + 24 + optional_size
    sections = []
    for _ in range(section_count):
        name = data[offset:offset + 8].rstrip(b"\0").decode("ascii", "replace")
        virtual_size, virtual_va, raw_size, raw_offset = (
            int.from_bytes(data[offset + 8 + i * 4:offset + 12 + i * 4], "little")
            for i in range(4)
        )
        sections.append(
            (name, image_base + virtual_va, virtual_size, raw_offset, raw_size)
        )
        offset += 40
    return image_base, sections

def _read_va(data: bytes, sections, va: int, count: int) -> bytes | None:
    for _name, section_va, virtual_size, raw_offset, raw_size in sections:
        if section_va <= va < section_va + max(virtual_size, raw_size):
            start = raw_offset + (va - section_va)
            if start + count <= len(data):
                return data[start:start + count]
    return None

def verify_native_symbol_evidence(
    model: dict[str, Any], root: Path
) -> dict[str, Any]:
    images: dict[str, tuple[bytes, Any]] = {}
    for build in model["builds"]:
        data = (root / build["binary_path"]).read_bytes()
        base, sections = _pe_sections(data, build["id"])
        _require(base == IMAGE_BASE,
                 f"{build['id']}: image base 0x{base:X} != 0x{IMAGE_BASE:X}")
        images[build["id"]] = (data, sections)

    november_data, november_sections = images["se_2006"]
    december_data, december_sections = images["game_2000"]

    november_by_name = {row[0]: row for row in november_sections}
    december_by_name = {row[0]: row for row in december_sections}
    _require(set(november_by_name) == set(december_by_name),
             "the two images do not share a section layout")
    for name, nov in november_by_name.items():
        dec = december_by_name[name]
        _require(nov[1] == dec[1],
                 f"section {name}: VA 0x{nov[1]:X} != 0x{dec[1]:X}")
        if name == ".text":
            _require(dec[2] - nov[2] == ENTRY_SHIFT,
                     f"section .text grew by 0x{dec[2] - nov[2]:X}, not "
                     f"0x{ENTRY_SHIFT:X}; the layout rule no longer holds")
        else:
            _require(nov[2] == dec[2],
                     f"section {name}: size changed; static data is not shared")
    _require(november_by_name[".rdata"][1] == CODE_DATA_BOUNDARY_VA,
             f".rdata must start at 0x{CODE_DATA_BOUNDARY_VA:X}")

    _require(_read_va(november_data, november_sections,
                      IDENTITY_LAST_ENTRY_VA, ENTRY_WINDOW_BYTES)
             == _read_va(december_data, december_sections,
                         IDENTITY_LAST_ENTRY_VA, ENTRY_WINDOW_BYTES),
             f"0x{IDENTITY_LAST_ENTRY_VA:X} must be present at the same VA in "
             "both images")
    _require(_read_va(november_data, november_sections,
                      SHIFTED_FIRST_ENTRY_NOVEMBER_VA, ENTRY_WINDOW_BYTES)
             == _read_va(december_data, december_sections,
                         SHIFTED_FIRST_ENTRY_NOVEMBER_VA + ENTRY_SHIFT,
                         ENTRY_WINDOW_BYTES),
             f"0x{SHIFTED_FIRST_ENTRY_NOVEMBER_VA:X} must pair with "
             f"0x{SHIFTED_FIRST_ENTRY_NOVEMBER_VA + ENTRY_SHIFT:X}")

    evidence: dict[str, Any] = {}

    def compare_entry_window(key: str, november_va: int, december_va: int,
                             prologue_bytes: int = 0) -> None:
        nov = _read_va(november_data, november_sections, november_va,
                       ENTRY_WINDOW_BYTES)
        dec = _read_va(december_data, december_sections, december_va,
                       ENTRY_WINDOW_BYTES)
        _require(nov is not None and dec is not None,
                 f"{key}: address is not mapped in one of the images")
        entry: dict[str, Any] = {"entry_windows_identical": nov == dec}
        if prologue_bytes:
            _require(prologue_bytes == ENTRY_WINDOW_BYTES,
                     f"{key}: prologue_bytes must equal {ENTRY_WINDOW_BYTES}")
            entry["november_prologue"] = nov
            entry["december_prologue"] = dec
        evidence[key] = entry

    for row in model["functions"]:
        compare_entry_window(row["cxx"][1:], row["se_2006_va"],
                             row["game_2000_va"],
                             row.get("prologue_bytes", 0))

    for row in model["native_symbols"]:
        key = native_symbol_enum_name(row["key"])
        if row["kind"] in CODE_KINDS:
            compare_entry_window(key, row["november_va"], row["december_va"],
                                 row.get("prologue_bytes", 0))
            continue
        evidence[key] = {"entry_windows_identical": None}
        if row["kind"] not in VALUE_BEARING_KINDS:
            continue
        for build_key, data, sections, field in (
            ("se_2006", november_data, november_sections, "november_value"),
            ("game_2000", december_data, december_sections, "december_value"),
        ):
            raw = _read_va(data, sections, row["november_va"], 4)
            _require(raw is not None, f"{row['key']}: unmapped in {build_key}")
            actual = int.from_bytes(raw, "little")
            _require(actual == row[field],
                     f"{row['key']}: {field} 0x{row[field]:X} != observed "
                     f"0x{actual:X} in {build_key}")
        if row["kind"] == "CodePointerStorage":
            _require(row["december_value"] - row["november_value"] == ENTRY_SHIFT,
                     f"{row['key']}: a code-pointer slot whose value does not "
                     f"shift by 0x{ENTRY_SHIFT:X} is not a shifted-entry "
                     "pointer and must be reclassified")

    for row in model["data_symbols"]:
        evidence[row["cxx"][1:]] = {"entry_windows_identical": None}

    return evidence

def _namespace_body(text: str, namespace: str) -> str:
    match = re.search(
        rf"namespace\s+{re.escape(namespace)}\s*\{{(.*?)^\}}",
        text,
        re.DOTALL | re.MULTILINE,
    )
    _require(match is not None, f"AddressMap.hpp: namespace {namespace} not found")
    return match.group(1)

def _parse_small_sum(expression: str) -> int:
    parts = [part.strip() for part in expression.split("+")]
    _require(all(re.fullmatch(r"(?:0[xX][0-9A-Fa-f]+|\d+)", part)
                 for part in parts),
             f"unsupported integer expression: {expression}")
    return sum(int(part, 0) for part in parts)

def validate_source_coverage(model: dict[str, Any], repo: Path = REPO) -> None:
    header_path = repo / "include/qoh99/AddressMap.hpp"
    header = header_path.read_text(encoding="utf-8")
    addr_body = _namespace_body(header, "addr")
    fn_body = _namespace_body(header, "fnaddr")

    address_rows = {
        match.group(1): int(match.group(2), 0)
        for match in re.finditer(
            r"constexpr\s+GameAddr\s+(k\w+)\s*=\s*"
            r"(0[xX][0-9A-Fa-f]+|\d+)\s*;",
            addr_body,
        )
    }
    manifest_addresses = {row["cxx"]: row["va"] for row in model["data_symbols"]}
    _require(address_rows == manifest_addresses,
             "AddressMap.hpp literal GameAddr declarations do not exactly match "
             "the 104 manifest data symbols")

    size_rows = {
        match.group(1): int(match.group(2), 0)
        for match in re.finditer(
            r"constexpr\s+std::uint32_t\s+(k\w+)\s*=\s*"
            r"(0[xX][0-9A-Fa-f]+|\d+)\s*;",
            addr_body,
        )
    }
    manifest_sizes = {row["cxx"]: row["value"] for row in model["sizes"]}
    _require(size_rows == manifest_sizes,
             "AddressMap.hpp size declarations do not exactly match the manifest")

    function_rows = {
        match.group(1): int(match.group(2), 0)
        for match in re.finditer(
            r"constexpr\s+GameAddr\s+(k\w+)\s*=\s*"
            r"(0[xX][0-9A-Fa-f]+|\d+)\s*;",
            fn_body,
        )
    }
    manifest_functions = {
        row["cxx"]: row["evidence_va"] for row in model["functions"]
    }
    _require(function_rows == manifest_functions,
            "AddressMap.hpp fnaddr declarations do not exactly match the "
            "manifest functions")

    address_cpp = (repo / "src/AddressMap.cpp").read_text(encoding="utf-8")
    entry_cxx = re.findall(
        r'^\s*\{\s*"[^"]+"\s*,\s*"[^"]+"\s*,\s*addr::(k\w+)\s*,',
        address_cpp,
        re.MULTILINE,
    )
    _require(len(entry_cxx) == len(model["data_symbols"]),
             f"AddressMap.cpp has {len(entry_cxx)} entries; "
             f"manifest has {len(model['data_symbols'])}")
    _require(set(entry_cxx) == set(manifest_addresses),
             "AddressMap.cpp entries do not cover every manifest data symbol exactly")
    _require("kNetIsHost" in entry_cxx,
             "AddressMap.cpp is missing the kNetIsHost runtime-query row")

    catalog_cpp = (repo / "src/GameplayRegionCatalog.cpp").read_text(
        encoding="utf-8"
    )
    catalog_rows = [
        (match.group(1), int(match.group(2), 0), int(match.group(3), 0),
         match.group(4))
        for match in re.finditer(
            r'\{\s*"([^"]+)"\s*,\s*(0[xX][0-9A-Fa-f]+)\s*,\s*'
            r'(0[xX][0-9A-Fa-f]+|\d+)\s*,\s*'
            r"GameplayRelevance::(\w+)",
            catalog_cpp,
        )
    ]
    manifest_regions = {
        row["name"]: (row["address"], row["size"], row["relevance"])
        for row in model["regions"]
    }
    catalog_region_map = {
        name: (address, size, relevance)
        for name, address, size, relevance in catalog_rows
    }
    _require(len(catalog_rows) == len(catalog_region_map)
             and catalog_region_map == manifest_regions,
             "GameplayRegionCatalog.cpp rows do not exactly match manifest regions")

    child_rows = []
    child_pattern = re.compile(
        r'\{\s*"(cmd_p[1-4])"\s*,\s*'
        r'((?:0[xX][0-9A-Fa-f]+|\d+)(?:\s*\+\s*'
        r'(?:0[xX][0-9A-Fa-f]+|\d+))?)\s*,\s*'
        r'(0[xX][0-9A-Fa-f]+|\d+)\s*,\s*'
        r"GameplayRelevance::(\w+)\s*,\s*(true|false)",
    )
    for match in child_pattern.finditer(catalog_cpp):
        child_rows.append((
            match.group(1),
            _parse_small_sum(match.group(2)),
            int(match.group(3), 0),
            match.group(4),
            match.group(5) == "true",
        ))
    manifest_children = [
        (row["name"], row["pointer_address"], row["pointee_size"],
         row["relevance"], row["nullable"])
        for row in model["pointer_children"]
    ]
    _require(child_rows == manifest_children,
             "GameplayRegionCatalog.cpp pointer children do not exactly match "
             "the manifest")

def _cpp_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'

def _banner(manifest_hash: str) -> list[str]:
    return [
        "// Generated by tools/generate_model.py from model/qoh99_manifest.toml.",
        f"// Manifest SHA-256: {manifest_hash}",
        "// Do not edit; run `python tools/generate_model.py`.",
        "#pragma once",
        "",
    ]

def render_address_constants(model: dict[str, Any], manifest_hash: str) -> bytes:
    lines = _banner(manifest_hash)
    lines += [
        "#include <cstddef>",
        "#include <cstdint>",
        '#include "qoh99/Types.hpp"',
        "",
        "namespace qoh99::generated_model {",
        "",
        "struct DataSymbol {",
        "    const char* cxxName;",
        "    const char* name;",
        "    const char* originalName;",
        "    GameAddr va;",
        "    std::uint32_t size;",
        "    const char* memoryClass;",
        "    const char* confidence;",
        "};",
        "",
        f"inline constexpr std::size_t kDataSymbolCount = "
        f"{len(model['data_symbols'])};",
        "inline constexpr DataSymbol kDataSymbols[] = {",
    ]
    for row in model["data_symbols"]:
        lines.append(
            "    { "
            f"{_cpp_string(row['cxx'])}, {_cpp_string(row['name'])}, "
            f"{_cpp_string(row['original'])}, 0x{row['va']:X}u, "
            f"0x{row['size']:X}u, {_cpp_string(row['mem_class'])}, "
            f"{_cpp_string(row['confidence'])} "
            "},"
        )
    lines += [
        "};",
        "",
        "namespace addr {",
    ]
    for row in model["data_symbols"]:
        lines.append(
            f"inline constexpr GameAddr {row['cxx']} = 0x{row['va']:X}u;"
        )
        for alias in row.get("legacy_aliases", []):
            lines.append(
                f"inline constexpr GameAddr {alias} = {row['cxx']};"
            )
    lines += [
        "} // namespace addr",
        "",
        "struct SizeConstant {",
        "    const char* cxxName;",
        "    const char* name;",
        "    std::uint32_t value;",
        "};",
        "",
        f"inline constexpr std::size_t kSizeConstantCount = "
        f"{len(model['sizes'])};",
        "inline constexpr SizeConstant kSizeConstants[] = {",
    ]
    for row in model["sizes"]:
        lines.append(
            f"    {{ {_cpp_string(row['cxx'])}, {_cpp_string(row['name'])}, "
            f"0x{row['value']:X}u }},"
        )
    lines += [
        "};",
        "",
        "namespace size {",
    ]
    for row in model["sizes"]:
        lines.append(
            f"inline constexpr std::uint32_t {row['cxx']} = "
            f"0x{row['value']:X}u;"
        )
    lines += [
        "} // namespace size",
        "",
        "} // namespace qoh99::generated_model",
        "",
    ]
    return "\n".join(lines).encode("utf-8")

def _function_enum_name(cxx_name: str) -> str:
    _require(cxx_name.startswith("k") and len(cxx_name) > 1,
             f"invalid function C++ name: {cxx_name}")
    return cxx_name[1:]

def render_function_catalog(model: dict[str, Any], manifest_hash: str) -> bytes:
    lines = _banner(manifest_hash)
    lines += [
        "#include <cstddef>",
        '#include "qoh99/Types.hpp"',
        '#include "qoh99/generated/BuildProfiles.hpp"',
        "",
        "namespace qoh99::generated_model {",
        "",
        "enum class FunctionId {",
        "    Unknown,",
    ]
    for row in model["functions"]:
        lines.append(f"    {_function_enum_name(row['cxx'])},")
    lines += [
        "};",
        "",
        "struct FunctionSymbol {",
        "    FunctionId id;",
        "    const char* cxxName;",
        "    const char* name;",
        "    const char* originalName;",
        "    GameAddr evidenceVa;",
        "    GameAddr game2000Va;",
        "    GameAddr se2006Va;",
        "    const char* runtimeMapping;",
        "};",
        "",
        f"inline constexpr std::size_t kFunctionSymbolCount = "
        f"{len(model['functions'])};",
        "inline constexpr FunctionSymbol kFunctions[] = {",
    ]
    for row in model["functions"]:
        lines.append(
            "    { "
            f"FunctionId::{_function_enum_name(row['cxx'])}, "
            f"{_cpp_string(row['cxx'])}, {_cpp_string(row['name'])}, "
            f"{_cpp_string(row['original'])}, 0x{row['evidence_va']:X}u, "
            f"0x{row['game_2000_va']:X}u, 0x{row['se_2006_va']:X}u, "
            f"{_cpp_string(row['runtime_mapping'])} "
            "},"
        )
    lines += [
        "};",
        "",
        "// constexpr so NativeSymbolCatalog.hpp can prove, at compile time, that",
        "// the consolidated table still resolves exactly what the runtime does.",
        "inline constexpr const FunctionSymbol* findFunction(FunctionId id) {",
        "    for (const auto& function : kFunctions)",
        "        if (function.id == id) return &function;",
        "    return nullptr;",
        "}",
        "",
        "inline constexpr GameAddr functionVaForBuild(const FunctionSymbol& function,",
        "                                    BuildId build) {",
        "    switch (build) {",
        "    case BuildId::Game2000: return function.game2000Va;",
        "    case BuildId::Se2006: return function.se2006Va;",
        "    }",
        "    return 0;",
        "}",
        "",
        "inline constexpr GameAddr resolveFunctionVa(BuildId build, FunctionId id) {",
        "    const FunctionSymbol* function = findFunction(id);",
        "    return function ? functionVaForBuild(*function, build) : 0;",
        "}",
        "",
        "} // namespace qoh99::generated_model",
        "",
    ]
    return "\n".join(lines).encode("utf-8")

def merged_native_symbols(model: dict[str, Any]) -> list[dict[str, Any]]:
    merged: list[dict[str, Any]] = []
    for row in model["functions"]:
        november = row["se_2006_va"]
        shift_class = ("IdentityLayout" if november <= IDENTITY_LAST_ENTRY_VA
                       else "ShiftedEntry")
        merged.append({
            "id": row["cxx"][1:],
            "key": row["name"],
            "original": row["original"],
            "kind": "FunctionEntry",
            "shift_class": shift_class,
            "runtime_use": row["runtime_use"],
            "november_va": november,
            "december_va": row["game_2000_va"],
            "november_value": 0,
            "december_value": 0,
            "function_id": row["cxx"][1:],
            "prologue_bytes": row.get("prologue_bytes", 0),
            "note": "Catalogued function entry; the December column is the "
                    "derived-and-checked " + shift_class + " peer.",
        })
    for row in model["data_symbols"]:
        merged.append({
            "id": row["cxx"][1:],
            "key": row["name"],
            "original": row["original"],
            "kind": "DataGlobal",
            "shift_class": "DataUnchanged",
            "runtime_use": "DataAccess",
            "november_va": row["va"],
            "december_va": row["va"],
            "november_value": 0,
            "december_value": 0,
            "function_id": "Unknown",
            "prologue_bytes": 0,
            "note": f"Shared static data ({row['mem_class']}, "
                    f"{row['confidence']}). Both builds place it at this VA; "
                    "the entry shift must never be applied to it.",
        })
    for row in model["native_symbols"]:
        merged.append({
            "id": native_symbol_enum_name(row["key"]),
            "key": row["key"],
            "original": row["original"],
            "kind": row["kind"],
            "shift_class": row["shift_class"],
            "runtime_use": row["runtime_use"],
            "november_va": row["november_va"],
            "december_va": row["december_va"],
            "november_value": row.get("november_value", 0),
            "december_value": row.get("december_value", 0),
            "function_id": row.get("function_id", "Unknown"),
            "prologue_bytes": row.get("prologue_bytes", 0),
            "note": row["note"],
        })
    return merged

def render_native_symbol_catalog(
    model: dict[str, Any], manifest_hash: str, evidence: dict[str, Any]
) -> bytes:
    merged = merged_native_symbols(model)
    lines = _banner(manifest_hash)
    lines += [
        "//",
        "// THE consolidated native address table. One row per native symbol,",
        "// carrying BOTH build columns, so no reader ever has to guess which",
        "// build a bare literal came from.",
        "//",
        "// Every December VA in here was DERIVED from its November VA by the",
        "// layout rule and then re-checked against both pinned executables --",
        "// none was copy-pasted. The rule is a FUNCTION-ENTRY rule only:",
        "//",
        "//   entry <= 0x472B80          December == November",
        "//   entry >= 0x4736C0          December == November + 0x20",
        "//   static data >= 0x499000    December == November (never shift it)",
        "//",
        "// It says nothing about an address interior to a function, because",
        "// sub_472B80 was itself edited and grew by 0x20; interiors therefore",
        "// move by an amount that is neither uniform nor 0x20. Those rows are",
        "// InteriorOfIdentityFunction or SemanticallyEdited, and the generator",
        "// refuses to infer their December column at all.",
        "//",
        "// December is DATA here and nothing else. Selection and permission to",
        "// hook live in RuntimeProfileRegistry, which still refuses every",
        "// profile that is not RuntimeQualification::Qualified. Resolving a",
        "// December address is not authorisation to execute it.",
        "#include <cstddef>",
        "#include <cstdint>",
        '#include "qoh99/Types.hpp"',
        '#include "qoh99/generated/AddressConstants.hpp"',
        '#include "qoh99/generated/BuildProfiles.hpp"',
        '#include "qoh99/generated/FunctionCatalog.hpp"',
        "",
        "namespace qoh99::generated_model {",
        "",
        "// The layout rule as constants, so it is stated once and asserted",
        "// below rather than re-typed at any call site.",
        f"inline constexpr GameAddr kImageBaseVa = 0x{IMAGE_BASE:X}u;",
        "inline constexpr GameAddr kIdentityLayoutLastEntryVa = "
        f"0x{IDENTITY_LAST_ENTRY_VA:X}u;",
        "inline constexpr GameAddr kShiftedLayoutFirstEntryNovemberVa = "
        f"0x{SHIFTED_FIRST_ENTRY_NOVEMBER_VA:X}u;",
        "inline constexpr std::uint32_t kShiftedLayoutEntryDelta = "
        f"0x{ENTRY_SHIFT:X}u;",
        "inline constexpr GameAddr kCodeDataBoundaryVa = "
        f"0x{CODE_DATA_BOUNDARY_VA:X}u;",
        "inline constexpr std::size_t kNativeEntryWindowBytesCompared = "
        f"{ENTRY_WINDOW_BYTES};",
        "",
        "enum class NativeSymbolKind {",
        "    FunctionEntry,",
        "    InteriorCode,",
        "    DataGlobal,",
        "    CodePointerStorage,  // slot never moves; stored VALUE shifts",
        "    DataTableWord,       // address identical, VALUE differs per build",
        "};",
        "",
        "enum class NativeShiftClass {",
        "    IdentityLayout,",
        "    ShiftedEntry,",
        "    DataUnchanged,",
        "    InteriorOfIdentityFunction,  // stated, never derived",
        "    SemanticallyEdited,          // stated, never derived",
        "};",
        "",
        "// Answers 'is this reachable today, and how'. Every RawLiteral* value",
        "// is also a live seam bypass and therefore a migration work item.",
        "enum class NativeRuntimeUse {",
        "    NotCallable,",
        "    SeamResolved,",
        "    // Address resolved through the profile seam, and NEVER called or",
        "    // hooked. It exists to be VERIFIED: the runtime compares the bytes",
        "    // at the resolved VA against the curated prologue below, and that",
        "    // comparison is the whole use. NotCallable would be a false",
        "    // statement (the runtime does resolve it) and SeamResolved would",
        "    // over-claim (nothing ever enters it).",
        "    SeamResolvedVerifyOnly,",
        "    DataAccess,",
        "    RawLiteralHookTarget,",
        "    RawLiteralCallTarget,",
        "    RawLiteralSiteTag,",
        "    RawLiteralRangeBound,",
        "    RawLiteralReturnKey,",
        "};",
        "",
        "// Observed by the generator from both hash-pinned images. Divergent",
        "// means the first kNativeEntryWindowBytesCompared bytes differ, which",
        "// in every observed case so far is an E8/E9 rel32 with exactly one end",
        "// crossing the layout split. It is a HOOKING fact, not a semantic one:",
        "// Identical never implies the two functions behave the same, and the",
        "// SemanticallyEdited rows are the proof of that.",
        "enum class EntryWindowComparison { NotApplicable, Identical, Divergent };",
        "",
        "// Per-build prologue signatures, OBSERVED rather than transcribed.",
        "// A Curated row's bytes were read by the generator out of the two",
        "// SHA-256-pinned executables at that row's own VA, so hand-editing one",
        "// byte fails `generate_model.py --check`. Most rows are still",
        "// NotCurated with a null payload, and that stays deliberate: inventing",
        "// signature bytes is exactly how a December port would fail closed for",
        "// a reason nobody could predict. Opt a row in with `prologue_bytes =",
        "// 16` in the manifest, never by typing bytes here.",
        "//",
        "// A consumer MUST treat NotCurated as a refusal, never as a pass. The",
        "// window is kNativeEntryWindowBytesCompared for every curated row, so",
        "// there is one window length with one meaning across the model.",
        "//",
        "// The December column is curated for the same rows and is DATA ONLY:",
        "// RuntimeProfileRegistry still refuses the December profile, and the",
        "// recovery qualification additionally refuses any resolved VA that is",
        "// not the November column. Curating it is not December progress.",
        "enum class PrologueSignatureStatus { NotCurated, Curated };",
        "",
        "struct PrologueSignature {",
        "    PrologueSignatureStatus status;",
        "    const std::uint8_t* bytes;",
        "    std::size_t size;",
        "};",
        "",
        "enum class NativeSymbolId {",
        "    Unknown,",
    ]
    for row in merged:
        lines.append(f"    {row['id']},")
    lines += [
        "};",
        "",
        "struct NativeSymbol {",
        "    NativeSymbolId id;",
        "    const char* key;",
        "    // WHICH BUILD THIS NAME CAME FROM DEPENDS ON THE ROW, and the",
        "    // split is not visible in the string. Rows projected from the",
        "    // manifest's [[functions]] inherit IDA's naming of the",
        "    // December-pinned evidence column, so a ShiftedEntry function",
        "    // reads 'sub_484BB0' beside novemberVa 0x484B90. Rows authored in",
        "    // [[native_symbols]] are named from November, so 'sub_47C0C0'",
        "    // sits beside novemberVa 0x47C0C0. NEVER cross-reference a",
        "    // by this name -- use novemberVa against the November executable",
        "    // and decemberVa against the December executable. The addresses are the",
        "    // contract; originalName is a human label only.",
        "    const char* originalName;",
        "    NativeSymbolKind kind;",
        "    NativeShiftClass shiftClass;",
        "    NativeRuntimeUse runtimeUse;",
        "    GameAddr novemberVa;",
        "    GameAddr decemberVa;",
        "    // Non-zero only for CodePointerStorage/DataTableWord, where the",
        "    // address is shared and the stored value is what differs.",
        "    std::uint32_t novemberValue;",
        "    std::uint32_t decemberValue;",
        "    FunctionId functionId;  // Unknown when the symbol has no seam entry",
        "    EntryWindowComparison entryWindows;",
        "    PrologueSignature novemberPrologue;",
        "    PrologueSignature decemberPrologue;",
        "    const char* note;",
        "};",
        "",
        "inline constexpr PrologueSignature kUncuratedPrologue{",
        "    PrologueSignatureStatus::NotCurated, nullptr, 0};",
        "",
    ]

    curated = [row for row in merged if row.get("prologue_bytes")]
    if curated:
        lines += [
            "// Curated prologue bytes. Each array is the first",
            "// kNativeEntryWindowBytesCompared bytes READ BY THE GENERATOR at",
            "// that row's own VA in the matching hash-pinned executable.",
        ]
        for row in curated:
            for column, field in (("November", "november_prologue"),
                                  ("December", "december_prologue")):
                observed = evidence[row["id"]][field]
                body = ", ".join(f"0x{byte:02X}" for byte in observed)
                lines.append(
                    f"inline constexpr std::uint8_t kPrologue{row['id']}"
                    f"{column}[] = {{ {body} }};"
                )
        lines.append("")

    lines += [
        f"inline constexpr std::size_t kNativeSymbolCount = {len(merged)};",
        "inline constexpr NativeSymbol kNativeSymbols[] = {",
    ]
    for row in merged:
        comparison = evidence[row["id"]]["entry_windows_identical"]
        if comparison is None:
            window = "NotApplicable"
        else:
            window = "Identical" if comparison else "Divergent"
        if row.get("prologue_bytes"):
            prologues = (
                "PrologueSignature{PrologueSignatureStatus::Curated, "
                f"kPrologue{row['id']}November, {ENTRY_WINDOW_BYTES}}}, "
                "PrologueSignature{PrologueSignatureStatus::Curated, "
                f"kPrologue{row['id']}December, {ENTRY_WINDOW_BYTES}}}, "
            )
        else:
            prologues = "kUncuratedPrologue, kUncuratedPrologue, "
        lines.append(
            "    { "
            f"NativeSymbolId::{row['id']}, {_cpp_string(row['key'])}, "
            f"{_cpp_string(row['original'])}, "
            f"NativeSymbolKind::{row['kind']}, "
            f"NativeShiftClass::{row['shift_class']}, "
            f"NativeRuntimeUse::{row['runtime_use']}, "
            f"0x{row['november_va']:X}u, 0x{row['december_va']:X}u, "
            f"0x{row['november_value']:X}u, 0x{row['december_value']:X}u, "
            f"FunctionId::{row['function_id']}, "
            f"EntryWindowComparison::{window}, "
            + prologues
            + f"{_cpp_string(row['note'])} "
            "},"
        )
    lines += [
        "};",
        "",
        "inline constexpr const NativeSymbol* findNativeSymbol(NativeSymbolId id) {",
        "    for (const auto& symbol : kNativeSymbols)",
        "        if (symbol.id == id) return &symbol;",
        "    return nullptr;",
        "}",
        "",
        "inline constexpr const NativeSymbol* findNativeSymbolByFunctionId(",
        "    FunctionId id) {",
        "    if (id == FunctionId::Unknown) return nullptr;",
        "    for (const auto& symbol : kNativeSymbols)",
        "        if (symbol.functionId == id) return &symbol;",
        "    return nullptr;",
        "}",
        "",
        "// Selection, never arithmetic. There is no shift applied anywhere in",
        "// this file; the shift only ever appears as an assertion.",
        "inline constexpr GameAddr nativeSymbolVaForBuild(const NativeSymbol& symbol,",
        "                                                BuildId build) {",
        "    switch (build) {",
        "    case BuildId::Game2000: return symbol.decemberVa;",
        "    case BuildId::Se2006: return symbol.novemberVa;",
        "    }",
        "    return 0;",
        "}",
        "",
        "inline constexpr GameAddr resolveNativeSymbolVa(BuildId build,",
        "                                               NativeSymbolId id) {",
        "    const NativeSymbol* symbol = findNativeSymbol(id);",
        "    return symbol ? nativeSymbolVaForBuild(*symbol, build) : 0;",
        "}",
        "",
        "inline constexpr GameAddr nativeSymbolNovemberVa(NativeSymbolId id) {",
        "    return resolveNativeSymbolVa(BuildId::Se2006, id);",
        "}",
        "",
        "inline constexpr GameAddr nativeSymbolDecemberVa(NativeSymbolId id) {",
        "    return resolveNativeSymbolVa(BuildId::Game2000, id);",
        "}",
        "",
        "// ---------------------------------------------------------------------",
        "// Proof 1: every row still obeys the layout rule it was derived from.",
        "// This is the rule CHECKED rather than APPLIED -- the two stated",
        "// classes are exempt by construction, which is the whole reason they",
        "// are separate classes.",
        "// ---------------------------------------------------------------------",
        "inline constexpr bool nativeCatalogObeysLayoutRule() {",
        "    for (const auto& s : kNativeSymbols) {",
        "        switch (s.shiftClass) {",
        "        case NativeShiftClass::IdentityLayout:",
        "            if (s.decemberVa != s.novemberVa) return false;",
        "            if (s.novemberVa > kIdentityLayoutLastEntryVa) return false;",
        "            break;",
        "        case NativeShiftClass::ShiftedEntry:",
        "            if (s.decemberVa != s.novemberVa + kShiftedLayoutEntryDelta)",
        "                return false;",
        "            if (s.novemberVa < kShiftedLayoutFirstEntryNovemberVa)",
        "                return false;",
        "            break;",
        "        case NativeShiftClass::DataUnchanged:",
        "            if (s.decemberVa != s.novemberVa) return false;",
        "            if (s.novemberVa < kCodeDataBoundaryVa) return false;",
        "            break;",
        "        case NativeShiftClass::InteriorOfIdentityFunction:",
        "            if (s.decemberVa != s.novemberVa) return false;",
        "            break;",
        "        case NativeShiftClass::SemanticallyEdited:",
        "            break;  // stated by a human; never derived, never checked",
        "        }",
        "    }",
        "    return true;",
        "}",
        'static_assert(nativeCatalogObeysLayoutRule(),',
        '              "a native symbol\'s December column contradicts the layout '
        'rule it claims");',
        "",
        "// ---------------------------------------------------------------------",
        "// Proof 2: the consolidated table cannot drift from the seam the",
        "// runtime actually resolves through today. If these two ever disagree,",
        "// the refactor stopped being behaviour-preserving.",
        "// ---------------------------------------------------------------------",
        "inline constexpr bool nativeCatalogAgreesWithFunctionCatalog() {",
        "    for (const auto& s : kNativeSymbols) {",
        "        if (s.functionId == FunctionId::Unknown) continue;",
        "        if (s.novemberVa",
        "            != resolveFunctionVa(BuildId::Se2006, s.functionId))",
        "            return false;",
        "        if (s.decemberVa",
        "            != resolveFunctionVa(BuildId::Game2000, s.functionId))",
        "            return false;",
        "    }",
        "    return true;",
        "}",
        'static_assert(nativeCatalogAgreesWithFunctionCatalog(),',
        '              "the consolidated catalog diverges from FunctionCatalog");',
        "",
        "// ---------------------------------------------------------------------",
        "// Proof 3: every November address, pinned to its literal value. This is",
        "// the behaviour-preservation proof for the runnable build -- a manifest",
        "// typo fails the BUILD instead of silently moving a live hook. Data",
        "// rows are additionally pinned to the addr:: constants the runtime",
        "// already uses, and function rows to the seam's own resolution.",
        "// ---------------------------------------------------------------------",
    ]
    for row in merged:
        lines.append(
            f"static_assert(nativeSymbolNovemberVa(NativeSymbolId::{row['id']})"
            f" == 0x{row['november_va']:X}u);"
        )
    lines.append("")
    for row in model["data_symbols"]:
        lines.append(
            f"static_assert(nativeSymbolNovemberVa(NativeSymbolId::"
            f"{row['cxx'][1:]}) == addr::{row['cxx']});"
        )
    lines.append("")
    for row in model["functions"]:
        lines.append(
            f"static_assert(resolveFunctionVa(BuildId::Se2006, FunctionId::"
            f"{row['cxx'][1:]}) == 0x{row['se_2006_va']:X}u);"
        )
    lines += [
        "",
        "// ---------------------------------------------------------------------",
        "// Proof 4: a prologue slot is either fully curated or fully empty.",
        "// The half-filled case is the dangerous one -- a Curated status with a",
        "// null payload would make a byte comparison silently compare nothing,",
        "// which is precisely the 'passes vacuously' failure this table exists",
        "// to prevent. Data rows can never be curated: static data has no",
        "// prologue, and its content is not even present in the file image.",
        "// ---------------------------------------------------------------------",
        "inline constexpr bool prologueSlotIsWellFormed(",
        "    const PrologueSignature& p, bool isCode) {",
        "    if (p.status != PrologueSignatureStatus::Curated)",
        "        return p.bytes == nullptr && p.size == 0;",
        "    return isCode && p.bytes != nullptr",
        "        && p.size == kNativeEntryWindowBytesCompared;",
        "}",
        "",
        "inline constexpr bool nativeCatalogPrologueSlotsAreWellFormed() {",
        "    for (const auto& s : kNativeSymbols) {",
        "        const bool isCode = s.kind == NativeSymbolKind::FunctionEntry",
        "            || s.kind == NativeSymbolKind::InteriorCode;",
        "        if (!prologueSlotIsWellFormed(s.novemberPrologue, isCode))",
        "            return false;",
        "        if (!prologueSlotIsWellFormed(s.decemberPrologue, isCode))",
        "            return false;",
        "        // Curation is per ROW, never per column: a row curated on one",
        "        // side only would let a reader think the other build had been",
        "        // looked at.",
        "        if ((s.novemberPrologue.status == PrologueSignatureStatus::Curated)",
        "            != (s.decemberPrologue.status",
        "                == PrologueSignatureStatus::Curated))",
        "            return false;",
        "    }",
        "    return true;",
        "}",
        "static_assert(nativeCatalogPrologueSlotsAreWellFormed(),",
        '              "a prologue signature slot is half-filled or curated on a '
        'data row");',
        "",
        "} // namespace qoh99::generated_model",
        "",
    ]
    return "\n".join(lines).encode("utf-8")

def _build_enum_name(build_id: str) -> str:
    return "".join(part[:1].upper() + part[1:] for part in build_id.split("_"))

def _runtime_profile_enum_name(profile_id: str) -> str:
    return "".join(
        part[:1].upper() + part[1:]
        for part in profile_id.replace("-", "_").split("_")
    )

def render_build_profiles(model: dict[str, Any], manifest_hash: str) -> bytes:
    lines = _banner(manifest_hash)
    lines += [
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace qoh99::generated_model {",
        "",
        "enum class BuildId {",
    ]
    for build in model["builds"]:
        lines.append(f"    {_build_enum_name(build['id'])},")
    lines += [
        "};",
        "",
        "enum class RuntimeProfileId {",
        "    Unknown,",
    ]
    for build in model["builds"]:
        lines.append(
            f"    {_runtime_profile_enum_name(build['runtime_profile_id'])},"
        )
    lines += [
        "};",
        "",
        "enum class RuntimeQualification {",
        "    Unknown,",
        "    Qualified,",
        "    CandidateUnqualified,",
        "};",
        "",
        "// What an INSTALLATION must satisfy for this build to run at all.",
        "// This is not installation provenance: both values are read and",
        "// compared by the game itself, so they decide whether the executable",
        "// starts.  The simulation probe is OURS: one installed file whose",
        "// digest says which build an installation is, checked alongside the",
        "// executable and nothing else.",
        "struct BuildContentPreconditions {",
        "    // Table word shared by both images at the same VA; only its VALUE",
        "    // differs, which is why a per-build address column alone cannot",
        "    // express this build difference.",
        "    std::uint32_t chrHeaderRevisionVa;",
        "    std::uint32_t chrHeaderRevision;",
        "    // The ONE installed file whose content identifies the build.",
        "    // Both builds ship a 735 596-byte Yosie.chr differing in a single",
        "    // byte, so this digest is exact - and, unlike the FILETIME gate it",
        "    // replaced, it is content-derived and survives a copy.",
        "    const char* simulationProbePath;    // normalized lower-case '/'",
        "    const char* simulationProbeSha256;  // upper-case hex, 64 chars",
        "};",
        "",
        "struct BuildProfile {",
        "    BuildId id;",
        "    const char* key;",
        "    const char* displayName;",
        "    const char* binarySha256;",
        "    std::uint64_t binarySize;",
        "    std::uint32_t coffTimestamp;",
        "    const char* dataLayoutId;",
        "    RuntimeProfileId runtimeProfileId;",
        "    const char* runtimeProfileKey;",
        "    RuntimeQualification runtimeQualification;",
        "    const char* simulationContentManifestPolicy;",
        "    bool executableIdentityEstablishesSimulationContent;",
        "    BuildContentPreconditions contentPreconditions;",
        "};",
        "",
        f"inline constexpr std::size_t kBuildCount = {len(model['builds'])};",
        "inline constexpr BuildProfile kBuilds[] = {",
    ]
    for build in model["builds"]:
        preconditions = build["content_preconditions"]
        rendered_preconditions = (
            "{ "
            f"0x{preconditions['chr_header_revision_va']:08X}u, "
            f"{preconditions['chr_header_revision']}u, "
            f"{_cpp_string(preconditions['simulation_probe_path'])}, "
            f"{_cpp_string(preconditions['simulation_probe_sha256'])}"
            " }"
        )
        lines.append(
            "    { "
            f"BuildId::{_build_enum_name(build['id'])}, "
            f"{_cpp_string(build['id'])}, {_cpp_string(build['display_name'])}, "
            f"{_cpp_string(build['binary_sha256'])}, "
            f"{build['binary_size']}ull, 0x{build['coff_timestamp']:08X}u, "
            f"{_cpp_string(build['data_layout_id'])}, "
            f"RuntimeProfileId::{_runtime_profile_enum_name(build['runtime_profile_id'])}, "
            f"{_cpp_string(build['runtime_profile_id'])}, "
            f"RuntimeQualification::{build['runtime_qualification']}, "
            f"{_cpp_string(build['simulation_content_manifest_policy'])}, "
            f"{'true' if build['executable_identity_establishes_simulation_content'] else 'false'}, "
            f"{rendered_preconditions} "
            "},"
        )
    lines += [
        "};",
        "",
        "// The build error messages quote when there is no observed",
        "// executable to name.  It is NOT 'the only build that may run' --",
        "// ask buildIsRuntimeQualified() for that.",
        "inline constexpr BuildId kSupportedRuntimeBuild = BuildId::Se2006;",
        "",
        "// Registering a build is not permission to run it.  These two",
        "// constexpr predicates read the emitted rows, and the assertions",
        "// below them are written from the generator's own pinned",
        "// runnable-build set rather than from the rows they check.  So a",
        "// manifest that qualified or demoted a build on its own would fail",
        "// manifest validation AND fail to compile here: the qualified set is",
        "// a structural property of the model, not a comment on a flag.",
        "constexpr bool buildIsRuntimeQualified(BuildId id) {",
        "    for (const auto& build : kBuilds)",
        "        if (build.id == id)",
        "            return build.runtimeQualification",
        "                == RuntimeQualification::Qualified;",
        "    return false;",
        "}",
        "",
        "constexpr std::size_t runtimeQualifiedBuildCount() {",
        "    std::size_t count = 0;",
        "    for (const auto& build : kBuilds)",
        "        if (build.runtimeQualification == RuntimeQualification::Qualified)",
        "            ++count;",
        "    return count;",
        "}",
        "",
    ]
    qualified = sorted(RUNTIME_QUALIFIED_BUILDS)
    lines.append(
        f"static_assert(runtimeQualifiedBuildCount() == {len(qualified)},"
    )
    lines.append(
        f'              "exactly {len(qualified)} QOH99 build(s) are runnable");'
    )
    for build in model["builds"]:
        enum_name = _build_enum_name(build["id"])
        if build["id"] in RUNTIME_QUALIFIED_BUILDS:
            lines.append(
                f"static_assert(buildIsRuntimeQualified(BuildId::{enum_name}),"
            )
            lines.append(
                f'              "{build["id"]} is a runnable build");'
            )
        else:
            lines.append(
                f"static_assert(!buildIsRuntimeQualified(BuildId::{enum_name}),"
            )
            lines.append(
                f'              "{build["id"]} is registered data only and must '
                'never become runnable");'
            )
    lines.append(
        "static_assert(buildIsRuntimeQualified(kSupportedRuntimeBuild),"
    )
    lines.append(
        '              "the supported runtime build must be a qualified one");'
    )
    lines += [
        "",
        "inline bool buildHashEquals(const char* a, const char* b) {",
        "    if (!a || !b) return false;",
        "    for (; *a && *b; ++a, ++b) {",
        "        char ca = (*a >= 'a' && *a <= 'f') ? char(*a - 'a' + 'A') : *a;",
        "        char cb = (*b >= 'a' && *b <= 'f') ? char(*b - 'a' + 'A') : *b;",
        "        if (ca != cb) return false;",
        "    }",
        "    return *a == *b;",
        "}",
        "",
        "inline const BuildProfile* findBuildBySha256(const char* sha256) {",
        "    for (const auto& build : kBuilds)",
        "        if (buildHashEquals(build.binarySha256, sha256)) return &build;",
        "    return nullptr;",
        "}",
        "",
        "inline const BuildProfile* findBuildByRuntimeProfileId(",
        "    RuntimeProfileId runtimeProfileId) {",
        "    for (const auto& build : kBuilds)",
        "        if (build.runtimeProfileId == runtimeProfileId) return &build;",
        "    return nullptr;",
        "}",
        "",
        "inline const BuildProfile* findBuildByCoffTimestamp(std::uint32_t timestamp) {",
        "    const BuildProfile* found = nullptr;",
        "    for (const auto& build : kBuilds) {",
        "        if (build.coffTimestamp != timestamp) continue;",
        "        if (found) return nullptr;  // ambiguous metadata never selects a profile",
        "        found = &build;",
        "    }",
        "    return found;",
        "}",
        "",
        "inline bool runtimeProfileAllowsHookInstall(const BuildProfile& build) {",
        "    return build.runtimeQualification == RuntimeQualification::Qualified;",
        "}",
        "",
        "inline const BuildProfile& supportedRuntimeBuild() {",
        "    for (const auto& build : kBuilds)",
        "        if (build.id == kSupportedRuntimeBuild) return build;",
        "    return kBuilds[0];",
        "}",
        "",
        "} // namespace qoh99::generated_model",
        "",
    ]
    return "\n".join(lines).encode("utf-8")

def render_gameplay_regions(model: dict[str, Any], manifest_hash: str) -> bytes:
    lines = _banner(manifest_hash)
    lines += [
        "#include <cstddef>",
        "#include <cstdint>",
        '#include "qoh99/Types.hpp"',
        "",
        "namespace qoh99::generated_model {",
        "",
        "enum class RegionSemantic { Gameplay, Render, Timing, External, Unknown };",
        "enum class RegionSafety {",
        "    PlainBytes, FixedImagePointers, ProcessLocalPointers,",
        "    ReadOnly, WallClock, Unknown",
        "};",
        "enum class PointerHashIdentity { OmitRawAddress, StableImage, OwnerRelative };",
        "",
        "struct RegionPolicy {",
        "    bool capture;",
        "    bool restore;",
        "    bool gameplayHash;",
        "    bool renderHash;",
        "};",
        "",
        "struct GameplayRegion {",
        "    const char* name;",
        "    GameAddr address;",
        "    std::uint32_t size;",
        "    const char* legacyRelevance;",
        "    RegionSemantic semantic;",
        "    RegionSafety safety;",
        "    RegionPolicy policy;",
        "    const char* confidence;",
        "};",
        "",
        f"inline constexpr std::size_t kGameplayRegionCount = "
        f"{len(model['regions'])};",
        "inline constexpr GameplayRegion kGameplayRegions[] = {",
    ]
    for row in model["regions"]:
        bool_cpp = lambda value: "true" if value else "false"
        lines.append(
            "    { "
            f"{_cpp_string(row['name'])}, 0x{row['address']:X}u, "
            f"0x{row['size']:X}u, {_cpp_string(row['relevance'])}, "
            f"RegionSemantic::{row['semantic']}, RegionSafety::{row['safety']}, "
            "{ "
            f"{bool_cpp(row['capture'])}, {bool_cpp(row['restore'])}, "
            f"{bool_cpp(row['gameplay_hash'])}, {bool_cpp(row['render_hash'])}"
            " }, "
            f"{_cpp_string(row['confidence'])} "
            "},"
        )
    lines += [
        "};",
        "",
        "struct PointerChild {",
        "    const char* name;",
        "    GameAddr pointerAddress;",
        "    std::uint32_t pointeeSize;",
        "    const char* legacyRelevance;",
        "    bool nullable;",
        "    const char* confidence;",
        "    PointerHashIdentity hashIdentity;",
        "};",
        "",
        f"inline constexpr std::size_t kPointerChildCount = "
        f"{len(model['pointer_children'])};",
        "inline constexpr PointerChild kPointerChildren[] = {",
    ]
    for row in model["pointer_children"]:
        nullable = "true" if row["nullable"] else "false"
        lines.append(
            "    { "
            f"{_cpp_string(row['name'])}, 0x{row['pointer_address']:X}u, "
            f"0x{row['pointee_size']:X}u, {_cpp_string(row['relevance'])}, "
            f"{nullable}, {_cpp_string(row['confidence'])}, "
            f"PointerHashIdentity::{row['hash_identity']} "
            "},"
        )
    lines += [
        "};",
        "",
        "} // namespace qoh99::generated_model",
        "",
    ]
    return "\n".join(lines).encode("utf-8")

def make_lock_document(
    model: dict[str, Any],
    manifest_bytes: bytes,
    generator_bytes: bytes,
    generated: dict[Path, bytes],
) -> dict[str, Any]:
    return {
        "schema_version": model["schema_version"],
        "generator_version": model["generator_version"],
        "manifest": {
            "path": "model/qoh99_manifest.toml",
            "sha256": sha256_bytes(manifest_bytes),
        },
        "generator": {
            "path": GENERATOR_REL.as_posix(),
            "sha256": sha256_bytes(generator_bytes),
        },
        "counts": {
            "data_symbols": len(model["data_symbols"]),
            "sizes": len(model["sizes"]),
            "functions": len(model["functions"]),
            "native_symbols": len(model["native_symbols"]),
            "merged_native_symbols": len(merged_native_symbols(model)),
            "regions": len(model["regions"]),
            "pointer_children": len(model["pointer_children"]),
            "builds": len(model["builds"]),
        },
        "builds": [
            {
                "id": build["id"],
                "binary_path": build["binary_path"],
                "binary_sha256": build["binary_sha256"],
                "binary_size": build["binary_size"],
                "coff_timestamp": f"0x{build['coff_timestamp']:08X}",
            }
            for build in model["builds"]
        ],
        "generated_outputs": {
            path.as_posix(): sha256_bytes(content)
            for path, content in sorted(
                generated.items(), key=lambda item: item[0].as_posix()
            )
        },
        "migration": model["migration"],
    }

def _without_comment_lines(body: bytes) -> bytes:
    lines = body.decode("utf-8").split("\n")
    return "\n".join(
        line for index, line in enumerate(lines)
        if index < 3 or not line.lstrip().startswith("//")
    ).encode("utf-8")


def expected_outputs(
    model: dict[str, Any],
    manifest_bytes: bytes,
    generator_bytes: bytes,
    evidence: dict[str, Any],
) -> dict[Path, bytes]:
    manifest_hash = sha256_bytes(manifest_bytes)
    generated = {
        OUTPUT_RELS[0]: render_address_constants(model, manifest_hash),
        OUTPUT_RELS[1]: render_function_catalog(model, manifest_hash),
        OUTPUT_RELS[2]: render_build_profiles(model, manifest_hash),
        OUTPUT_RELS[3]: render_gameplay_regions(model, manifest_hash),
        OUTPUT_RELS[4]: render_native_symbol_catalog(
            model, manifest_hash, evidence),
    }
    generated = {path: _without_comment_lines(body) for path, body in generated.items()}
    lock = make_lock_document(model, manifest_bytes, generator_bytes, generated)
    generated[LOCK_REL] = (
        json.dumps(lock, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    return generated

def validate_lock_document(
    document: dict[str, Any],
    model: dict[str, Any],
    manifest_bytes: bytes,
    generator_bytes: bytes,
    generated_without_lock: dict[Path, bytes],
) -> None:
    expected = make_lock_document(
        model, manifest_bytes, generator_bytes, generated_without_lock
    )
    _require(document == expected,
             "manifest.lock.json provenance is stale or inconsistent")

def find_drift(outputs: dict[Path, bytes], repo: Path = REPO) -> list[str]:
    drift: list[str] = []
    for relative, expected in outputs.items():
        path = repo / relative
        if not path.is_file():
            drift.append(f"missing generated output: {relative.as_posix()}")
            continue
        actual = path.read_bytes()
        if actual != expected:
            drift.append(
                f"stale generated output: {relative.as_posix()} "
                f"(actual {sha256_bytes(actual)}, expected {sha256_bytes(expected)})"
            )
    return drift

def write_outputs(outputs: dict[Path, bytes], repo: Path = REPO) -> None:
    for relative, content in outputs.items():
        path = repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)

def check_against_lock(
    model: dict[str, Any],
    manifest_bytes: bytes,
    generator_bytes: bytes,
    repo: Path,
) -> int:
    lock = json.loads((repo / LOCK_REL).read_text(encoding="utf-8"))
    generated = {
        Path(rel): (repo / rel).read_bytes()
        for rel in lock["generated_outputs"]
    }
    validate_lock_document(lock, model, manifest_bytes, generator_bytes, generated)
    print("[model-gen] CHECK PASSED against the lock (no executables supplied)")
    return 0

def run(manifest_path: Path, repo: Path, check: bool) -> int:
    model, manifest_bytes = load_manifest(manifest_path)
    validate_manifest(model)
    validate_source_coverage(model, repo)
    generator_bytes = (repo / GENERATOR_REL).read_bytes()
    root = evidence_root()
    if root is None:
        _require(check, "generating requires QOH99_RE_EVIDENCE_ROOT")
        return check_against_lock(model, manifest_bytes, generator_bytes, repo)
    verify_build_artifacts(model, root)
    evidence = verify_native_symbol_evidence(model, root)
    outputs = expected_outputs(model, manifest_bytes, generator_bytes, evidence)
    if check:
        drift = find_drift(outputs, repo)
        if drift:
            raise ManifestError("\n".join(drift))
        print(
            "[model-gen] CHECK PASSED "
            f"({len(model['data_symbols'])} data, {len(model['functions'])} "
            f"functions, {len(merged_native_symbols(model))} consolidated "
            f"native symbols, {len(model['regions'])} regions, "
            f"{len(model['builds'])} exact builds)"
        )
        return 0
    write_outputs(outputs, repo)
    print(
        f"[model-gen] generated {len(outputs)} checked-in outputs from "
        f"{manifest_path}"
    )
    return 0

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--manifest", type=Path, default=DEFAULT_MANIFEST,
        help="manifest path (default: model/qoh99_manifest.toml)",
    )
    parser.add_argument(
        "--repo", type=Path, default=REPO,
        help="qoh99_re root (default: inferred from this script)",
    )
    parser.add_argument(
        "--check", action="store_true",
        help="verify without writing; fail on stale/missing output",
    )
    args = parser.parse_args()
    try:
        return run(args.manifest.resolve(), args.repo.resolve(), args.check)
    except (ManifestError, FileNotFoundError, OSError) as exc:
        print(f"[model-gen] ERROR: {exc}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main())
