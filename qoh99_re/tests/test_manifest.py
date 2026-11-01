#!/usr/bin/env python3

from __future__ import annotations

import copy
import json
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

import generate_model as modelgen

def expect_manifest_error(action, label: str) -> None:
    try:
        action()
    except modelgen.ManifestError:
        return
    raise AssertionError(f"{label}: expected ManifestError")

def main() -> int:
    model, manifest_bytes = modelgen.load_manifest(
        REPO / "model/qoh99_manifest.toml"
    )
    modelgen.validate_manifest(model)
    modelgen.validate_source_coverage(model, REPO)
    root = modelgen.evidence_root()
    if root is None:
        print("SKIP: QOH99_RE_EVIDENCE_ROOT is not set")
        return 77
    modelgen.verify_build_artifacts(model, root)
    evidence = modelgen.verify_native_symbol_evidence(model, root)

    assert len(model["data_symbols"]) == 105
    assert len(model["functions"]) == 44
    assert len(model["native_symbols"]) == 106
    assert len(modelgen.merged_native_symbols(model)) == 255

    curated = {
        row["cxx"][1:] for row in model["functions"]
        if row.get("prologue_bytes")
    } | {
        modelgen.native_symbol_enum_name(row["key"])
        for row in model["native_symbols"] if row.get("prologue_bytes")
    }
    assert curated == {
        "ManualRecoveryEvaluator", "CommandMatch", "FighterRecordInput",
        "ThrowTechEvaluator", "GuardContactResolver", "HitContactResolver",
        "NaturalActionTerminal", "EditedActorTransition",
        "AttackContactResolver", "DamageCriticalScaler",
        "GuardActionRequest", "FaultlessButtonDetector",
        "ReduceMashDetector",
    }, curated
    november_image = (
        root / next(row for row in model["builds"]
                    if row["id"] == "se_2006")["binary_path"]
    ).read_bytes()
    _, november_sections = modelgen._pe_sections(november_image, "se_2006")
    for row in model["functions"]:
        if not row.get("prologue_bytes"):
            continue
        key = row["cxx"][1:]
        assert evidence[key]["november_prologue"] == modelgen._read_va(
            november_image, november_sections, row["se_2006_va"],
            modelgen.ENTRY_WINDOW_BYTES), key
    for key, entry in evidence.items():
        assert ("november_prologue" in entry) == (key in curated), key

    assert {row["id"] for row in model["builds"]} == {
        "game_2000", "se_2006"
    }
    assert next(
        row for row in model["data_symbols"] if row["cxx"] == "kNetIsHost"
    )["va"] == 0x66EE00
    assert next(
        row for row in model["builds"] if row["id"] == "se_2006"
    )["runtime_profile_id"] == "qoh99-se1126"
    december = next(row for row in model["builds"] if row["id"] == "game_2000")
    assert december["runtime_profile_id"] == "qoh99-december-candidate"
    assert december["runtime_qualification"] == "Qualified"
    assert not december["executable_identity_establishes_simulation_content"]

    generator_bytes = (REPO / modelgen.GENERATOR_REL).read_bytes()
    outputs = modelgen.expected_outputs(
        model, manifest_bytes, generator_bytes, evidence
    )
    assert not modelgen.find_drift(outputs, REPO)

    lock = json.loads((REPO / modelgen.LOCK_REL).read_text(encoding="utf-8"))
    without_lock = {
        path: content for path, content in outputs.items()
        if path != modelgen.LOCK_REL
    }
    modelgen.validate_lock_document(
        lock, model, manifest_bytes, generator_bytes, without_lock
    )

    missing_host = copy.deepcopy(model)
    missing_host["data_symbols"] = [
        row for row in missing_host["data_symbols"]
        if row["cxx"] != "kNetIsHost"
    ]
    missing_host["expected_counts"]["data_symbols"] = 104
    expect_manifest_error(
        lambda: modelgen.validate_manifest(missing_host),
        "missing kNetIsHost",
    )

    def mutate_native(key: str, **fields):
        mutated = copy.deepcopy(model)
        row = next(r for r in mutated["native_symbols"] if r["key"] == key)
        row.update(fields)
        return mutated

    expect_manifest_error(
        lambda: modelgen.validate_manifest(
            mutate_native("paletteApply", december_va=0x45B5B0)),
        "identity-layout entry given a shifted December column",
    )
    expect_manifest_error(
        lambda: modelgen.validate_manifest(
            mutate_native("inputSystemKeys", december_va=0x47C0C0)),
        "shifted entry given an unshifted December column",
    )
    expect_manifest_error(
        lambda: modelgen.validate_manifest(
            mutate_native("bgmOwnedCallsiteA",
                          shift_class="IdentityLayout")),
        "interior address made derivable by the entry rule",
    )
    expect_manifest_error(
        lambda: modelgen.validate_manifest(
            mutate_native("dataTableWord4A71E0",
                          shift_class="ShiftedEntry",
                          december_va=0x4A7200)),
        "static data classified as a shifted code entry",
    )
    expect_manifest_error(
        lambda: modelgen.validate_manifest(
            mutate_native("paletteApply", shift_class="DataUnchanged")),
        "code address classified DataUnchanged",
    )
    expect_manifest_error(
        lambda: modelgen.verify_native_symbol_evidence(
            mutate_native("codePointer499914", december_value=0x492FA0), root),
        "code-pointer value contradicted by the pinned images",
    )
    expect_manifest_error(
        lambda: modelgen.verify_native_symbol_evidence(
            mutate_native("dataTableWord4A71E0", november_value=7), root),
        "table word value contradicted by the pinned images",
    )

    def mutate_build(build_id: str, **fields):
        mutated = copy.deepcopy(model)
        row = next(r for r in mutated["builds"] if r["id"] == build_id)
        row.update(fields)
        return mutated

    def mutate_preconditions(build_id: str, **fields):
        mutated = copy.deepcopy(model)
        row = next(r for r in mutated["builds"] if r["id"] == build_id)
        row["content_preconditions"] = dict(row["content_preconditions"])
        row["content_preconditions"].update(fields)
        return mutated

    expect_manifest_error(
        lambda: modelgen.validate_manifest(
            mutate_build("game_2000",
                         runtime_qualification="CandidateUnqualified")),
        "December demoted by a manifest edit alone",
    )
    expect_manifest_error(
        lambda: modelgen.validate_manifest(
            mutate_build("se_2006",
                         runtime_qualification="CandidateUnqualified")),
        "November demoted by a manifest edit alone",
    )

    expect_manifest_error(
        lambda: modelgen.validate_manifest(
            mutate_preconditions("game_2000", chr_header_revision=5)),
        "December content precondition contradicting the observed table word",
    )
    expect_manifest_error(
        lambda: modelgen.validate_manifest(
            mutate_preconditions("se_2006", chr_header_revision_va=0x4A71E4)),
        "content precondition pointing at the wrong table word",
    )
    expect_manifest_error(
        lambda: modelgen.validate_manifest(
            mutate_preconditions(
                "se_2006", simulation_probe_sha256=
                "F19BD19FBB1FC26D6CEB8EC6FB4955489CABCA256A298AD9A1251DF6F16DCCEE")),
        "November claiming a probe it must not have",
    )
    expect_manifest_error(
        lambda: modelgen.validate_manifest(
            mutate_preconditions(
                "game_2000",
                simulation_probe_sha256=
                "a4ad728f7e37856138b30dc8d9cd842df142540e8eb5f8106743f26327e9f9e2")),
        "a lower-case digest, which would never compare equal",
    )
    expect_manifest_error(
        lambda: modelgen.validate_manifest(
            mutate_preconditions(
                "game_2000", simulation_probe_sha256="A4AD728F")),
        "a truncated digest",
    )
    expect_manifest_error(
        lambda: modelgen.validate_manifest(
            mutate_preconditions("game_2000", simulation_probe_sha256="")),
        "December with no probe digest at all",
    )
    expect_manifest_error(
        lambda: modelgen.validate_manifest(
            mutate_preconditions(
                "game_2000",
                simulation_probe_path=r"YOSIE\YOSIE.CHR")),
        "un-normalized probe path",
    )

    bad_november = copy.deepcopy(model)
    bad_november["functions"] = copy.deepcopy(bad_november["functions"])
    row = next(r for r in bad_november["functions"]
               if r["cxx"] == "kReadLogicalInput")
    row["se_2006_va"] = 0x484B70
    expect_manifest_error(
        lambda: modelgen.validate_manifest(bad_november),
        "November function address moved out from under the layout rule",
    )

    stale_lock = copy.deepcopy(lock)
    stale_lock["manifest"]["sha256"] = "0" * 64
    expect_manifest_error(
        lambda: modelgen.validate_lock_document(
            stale_lock, model, manifest_bytes, generator_bytes, without_lock
        ),
        "stale provenance",
    )

    with tempfile.TemporaryDirectory(prefix="qoh99-model-test-") as temp:
        temp_repo = Path(temp)
        modelgen.write_outputs(outputs, temp_repo)
        assert not modelgen.find_drift(outputs, temp_repo)
        drift_target = temp_repo / modelgen.OUTPUT_RELS[0]
        drift_target.write_bytes(drift_target.read_bytes() + b"\n// drift\n")
        drift = modelgen.find_drift(outputs, temp_repo)
        assert any("AddressConstants.hpp" in issue for issue in drift)

    print(
        "[manifest-test] PASS: parse, 105/43/254 coverage, exact build hashes, "
        "netIsHost, derived-and-checked December columns, per-build values "
        "verified against both images, curated prologues re-read from the "
        "November image, qualification unchangeable from the manifest alone, "
        "build content "
        "preconditions bound to the observed table word, generated drift, and "
        "stale provenance"
    )
    return 0

if __name__ == "__main__":
    sys.exit(main())
