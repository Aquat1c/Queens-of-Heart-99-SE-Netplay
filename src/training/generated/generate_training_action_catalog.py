#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import pathlib
import re
import subprocess
import tempfile

RESOURCES = (
    "SAORI", "MIZUHO", "RURIKO", "HATUNE", "KAEDE", "AZUSA",
    "CHIZURU", "MIZUKI", "AKARI", "SERIKA", "TOMOKO", "SIHO",
    "AOI", "YUKI", "SERIO", "MULTI", "KOTONE", "REMY", "RIO",
    "AYAKA", "RINA", "O_AKARI", "M_MULTI", "O_CHIZURU", "KANAKO",
    "C_SERIKA", "S_SIHO", "S_KOTONE", "LF97_AOI", "H_HATUNE",
    "TIRIA", "R_AYAKA", "CORIN", "YOSIE", "EDIFEL",
)

PROFILE_KEYS = ("qoh99-se1126", "qoh99-withse")
STATE_COUNT = 256
EXPECTED_AUTHORED = 3826
EXPECTED_DISTINCT_IDS = 175
EXPECTED_NATIVE_BEHAVIOR_RULES = 113
EXPECTED_NATIVE_BEHAVIOR_SOURCE_STATES = 69
EXPECTED_NATIVE_TRIGGER_COUNTS = {0: 13, 1: 73, 2: 20, 3: 4, 4: 3}
EXPECTED_NATIVE_EFFECT_COUNTS = {1: 13, 2: 94, 3: 3, 4: 2, 5: 1}
EXPECTED_NATIVE_CAPTURE_RULES = 15

MOVE_CATEGORY = {
    "neutral": 0,
    "movement": 1,
    "normal": 2,
    "command normal": 3,
    "throw": 4,
    "special": 5,
    "super": 6,
    "follow-up": 7,
    "defensive": 8,
    "partner command": 9,
}

MOVE_CONTEXT = {
    "ground": 0,
    "air": 1,
    "ground/air": 2,
    "close": 3,
    "while guarding": 4,
    "follow-up state": 5,
}

MOVE_FLAG_AIR_NOTATION = 1 << 0
MOVE_FLAG_CLOSE_QUALIFIER = 1 << 1
MOVE_FLAG_PARTNER_ACTOR = 1 << 2
MOVE_FLAG_MULTI_BUTTON = 1 << 3
MOVE_FLAG_MODE_DEPENDENT = 1 << 4
MOVE_FLAG_FOLLOW_UP = 1 << 5
MOVE_FLAG_REPEATED_BUTTON = 1 << 6
MOVE_FLAG_GUARD_CANCEL_ELIGIBLE = 1 << 7
MOVE_FLAG_ACTION_UNKNOWN = 1 << 8

MOVE_UNKNOWN_ACTION = 0xFFFF
MOVE_REPEAT_TAP_COUNT = 4
MOVE_MAXIMUM_SEQUENCE_STEPS = 9
EXPECTED_TRIGGER_MOVE_ROWS = 1159
EXPECTED_TRIGGER_MOVE_STEPS = 4685
EXPECTED_TRIGGER_MOVE_EXCLUDED = 39
EXPECTED_TRIGGER_MOVE_MAXIMUM_PER_CHARACTER = 54

LOGICAL_UP = 0x10000
LOGICAL_DOWN = 0x20000
LOGICAL_BACK = 0x40000
LOGICAL_FORWARD = 0x80000
LOGICAL_BUTTON = {"A": 0x1, "B": 0x2, "C": 0x4, "D": 0x8}
LOGICAL_DIRECTION = {
    "1": LOGICAL_DOWN | LOGICAL_BACK,
    "2": LOGICAL_DOWN,
    "3": LOGICAL_DOWN | LOGICAL_FORWARD,
    "4": LOGICAL_BACK,
    "5": 0,
    "6": LOGICAL_FORWARD,
    "7": LOGICAL_UP | LOGICAL_BACK,
    "8": LOGICAL_UP,
    "9": LOGICAL_UP | LOGICAL_FORWARD,
}

SEMANTIC = {
    "unused": 0,
    "unknown": 1,
    "neutral": 2,
    "movement": 3,
    "attack": 4,
    "guard": 5,
    "hit reaction": 6,
    "knockdown": 7,
    "recovery": 8,
    "throw": 9,
    "throw tech counterpart": 10,
    "throw tech": 11,
    "throw reception": 12,
    "cinematic/system": 13,
}

EXIT_UNUSED = 0
EXIT_UNKNOWN = 1
EXIT_TERMINAL = 2
EXIT_LOOP = 3
EXIT_LANDING = 4

FLAG_AUTHORED = 1 << 0
FLAG_TERMINAL = 1 << 1
FLAG_LOOP = 1 << 2
FLAG_EXPLICIT_LOOP = 1 << 3
FLAG_LANDING = 1 << 4
FLAG_ATTACK = 1 << 5
FLAG_CATALOG_MOVE = 1 << 6
FLAG_BROAD_THROW_DENY = 1 << 7
FLAG_GROUNDED_RUNTIME_LANDING = 1 << 8
FLAG_TIMING_DEPENDS_ON_LANDING = 1 << 9
FLAG_ACTIVE_UNTIL_LANDING = 1 << 10

NATIVE_TRIGGERS = {
    "on contact": 0,
    "on hit": 1,
    "on block or clash": 2,
    "without a hit": 3,
    "paired fighter version": 4,
}

NATIVE_EFFECTS = {
    "continue from animation frame": 1,
    "continue with follow-up": 2,
    "use alternate version": 3,
    "finish move": 4,
    "update character state": 5,
}

NATIVE_NO_BYTE = 0xFF
NATIVE_SOURCE_ACTION_SHIFT = 6
NATIVE_TRIGGER_SHIFT = 14
NATIVE_EFFECT_SHIFT = 17
NATIVE_CONTACT_SHIFT = 20
NATIVE_TRIGGER_FRAME_SHIFT = 27
NATIVE_TARGET_ACTION_SHIFT = 35
NATIVE_TARGET_FRAME_SHIFT = 43
NATIVE_RESET_TIMER_SHIFT = 51
NATIVE_CLEAR_MOTION_SHIFT = 52
NATIVE_CAPTURE_SHIFT = 53

def parse_bool(value: object) -> bool:
    if isinstance(value, bool):
        return value
    return str(value).strip().lower() in ("1", "true", "yes")

def find_chr(root: pathlib.Path, resource: str) -> pathlib.Path:
    matches = [p for p in root.rglob("*.chr") if p.stem.upper() == resource]
    if len(matches) != 1:
        raise RuntimeError(
            f"expected one {resource}.CHR below {root}, found {len(matches)}")
    return matches[0]

def export_profile(cli: pathlib.Path, root: pathlib.Path,
                   destination: pathlib.Path) -> dict[str, tuple[dict, dict]]:
    result: dict[str, tuple[dict, dict]] = {}
    for resource in RESOURCES:
        out = destination / resource
        subprocess.run(
            [str(cli), "export-catalog", "--chr", str(find_chr(root, resource)),
             "--out", str(out)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        document = json.loads((out / "character.json").read_text("utf-8"))
        with (out / "actions.csv").open(newline="", encoding="utf-8-sig") as stream:
            rows = {int(row["action_id"]): row for row in csv.DictReader(stream)}
        if document["resource"] != resource or len(document["actions"]) != STATE_COUNT:
            raise RuntimeError(f"unexpected Attack Studio export for {resource}")
        result[resource] = (document, rows)
    return result

def semantic_for(action: dict) -> int:
    if not action["authored"]:
        return SEMANTIC["unused"]

    action_id = int(action["id"])
    if action_id == 72:
        return SEMANTIC["throw tech counterpart"]
    if action_id == 73:
        return SEMANTIC["throw tech"]
    role = str(action["role"]).lower()
    if role in ("normal attack", "special attack"):
        return SEMANTIC["attack"]
    if role == "wall state":
        return SEMANTIC["knockdown"]
    if role in SEMANTIC:
        return SEMANTIC[role]
    return SEMANTIC["unknown"]

def pack_action(action: dict, csv_row: dict) -> int:
    action_id = int(action["id"])
    authored = bool(action["authored"])
    terminal = authored and parse_bool(csv_row["terminal"])
    loops = authored and parse_bool(csv_row["loops"])
    explicit_loop = authored and parse_bool(csv_row["explicit_loop"])
    active_until_landing = authored and bool(action["activeUntilLanding"])
    timing_depends = authored and bool(action["timingDependsOnLanding"])
    grounded_runtime = authored and bool(action["groundedRuntimeResolved"])
    landing_branches = authored and bool(action["landingRecoveryBranches"])
    landing = (active_until_landing or timing_depends or grounded_runtime or
               landing_branches)

    flags = 0
    if authored:
        flags |= FLAG_AUTHORED
    if terminal:
        flags |= FLAG_TERMINAL
    if loops:
        flags |= FLAG_LOOP
    if explicit_loop:
        flags |= FLAG_EXPLICIT_LOOP
    if landing:
        flags |= FLAG_LANDING
    if authored and bool(action["hasAttackRecords"]):
        flags |= FLAG_ATTACK
    if authored and bool(action["moves"]):
        flags |= FLAG_CATALOG_MOVE
    if action_id >= 200:
        flags |= FLAG_BROAD_THROW_DENY
    if grounded_runtime:
        flags |= FLAG_GROUNDED_RUNTIME_LANDING
    if timing_depends:
        flags |= FLAG_TIMING_DEPENDS_ON_LANDING
    if active_until_landing:
        flags |= FLAG_ACTIVE_UNTIL_LANDING

    if not authored:
        exit_class = EXIT_UNUSED
    elif landing:
        exit_class = EXIT_LANDING
    elif terminal:
        exit_class = EXIT_TERMINAL
    elif loops:
        exit_class = EXIT_LOOP
    else:
        exit_class = EXIT_UNKNOWN

    return semantic_for(action) | (exit_class << 4) | (flags << 8)

def classify(profile: dict[str, tuple[dict, dict]]) -> tuple[list[list[int]], str]:
    table: list[list[int]] = []
    canonical = bytearray()
    for resource in RESOURCES:
        document, rows = profile[resource]
        packed_row = []
        for action in document["actions"]:
            action_id = int(action["id"])
            value = pack_action(action, rows[action_id])
            packed_row.append(value)
            canonical.extend(value.to_bytes(4, "little"))
        table.append(packed_row)
    return table, hashlib.sha256(canonical).hexdigest().upper()

def native_byte(value: object, field: str, rule_id: str) -> int:
    if value is None:
        return NATIVE_NO_BYTE
    result = int(value)
    if not 0 <= result < NATIVE_NO_BYTE:
        raise RuntimeError(f"{rule_id}: {field} does not fit the compact catalog")
    return result

def classify_native_behaviors(
        profile: dict[str, tuple[dict, dict]]) -> tuple[
            list[int], list[int], list[tuple], str]:
    packed_rules: list[int] = []
    resource_ranges: list[int] = []
    signatures: list[tuple] = []
    source_states: set[tuple[int, int]] = set()

    for resource_index, resource in enumerate(RESOURCES):
        begin = len(packed_rules)
        document, _ = profile[resource]
        for action in document["actions"]:
            source_action = int(action["id"])
            for rule in action["nativeMoveBehavior"]:
                rule_id = str(rule["id"])
                trigger = NATIVE_TRIGGERS[str(rule["trigger"])]
                effect = NATIVE_EFFECTS[str(rule["effect"])]
                contact_mask = int(rule["nativeContactMask"])
                trigger_frame = native_byte(
                    rule["nativeTriggerAnimationFrame"], "trigger frame", rule_id)
                target_action = native_byte(
                    rule["targetAction"], "target action", rule_id)
                target_frame = native_byte(
                    rule["nativeTargetAnimationFrame"], "target frame", rule_id)
                resets_timer = bool(rule["resetsFrameTimer"])
                clears_motion = bool(rule["clearsHorizontalMotion"])
                capture = "capture" in rule_id

                if not 0 <= resource_index < 64 or not 0 <= source_action < 256:
                    raise RuntimeError(f"{rule_id}: source does not fit compact catalog")
                if not 0 <= contact_mask < 128:
                    raise RuntimeError(f"{rule_id}: contact mask does not fit catalog")

                packed = resource_index
                packed |= source_action << NATIVE_SOURCE_ACTION_SHIFT
                packed |= trigger << NATIVE_TRIGGER_SHIFT
                packed |= effect << NATIVE_EFFECT_SHIFT
                packed |= contact_mask << NATIVE_CONTACT_SHIFT
                packed |= trigger_frame << NATIVE_TRIGGER_FRAME_SHIFT
                packed |= target_action << NATIVE_TARGET_ACTION_SHIFT
                packed |= target_frame << NATIVE_TARGET_FRAME_SHIFT
                packed |= int(resets_timer) << NATIVE_RESET_TIMER_SHIFT
                packed |= int(clears_motion) << NATIVE_CLEAR_MOTION_SHIFT
                packed |= int(capture) << NATIVE_CAPTURE_SHIFT
                packed_rules.append(packed)
                source_states.add((resource_index, source_action))
                signatures.append((
                    rule_id, resource_index, source_action, trigger, effect,
                    contact_mask, trigger_frame, target_action, target_frame,
                    resets_timer, clears_motion, capture,
                ))

        count = len(packed_rules) - begin
        if begin >= 256 or count >= 256:
            raise RuntimeError("native behavior resource range no longer fits")
        resource_ranges.append(begin | (count << 8))

    if len(packed_rules) != EXPECTED_NATIVE_BEHAVIOR_RULES or \
            len(source_states) != EXPECTED_NATIVE_BEHAVIOR_SOURCE_STATES:
        raise RuntimeError(
            "native behavior invariant changed: "
            f"rules={len(packed_rules)}, source states={len(source_states)}")
    trigger_counts = {
        trigger: sum(signature[3] == trigger for signature in signatures)
        for trigger in NATIVE_TRIGGERS.values()
    }
    effect_counts = {
        effect: sum(signature[4] == effect for signature in signatures)
        for effect in NATIVE_EFFECTS.values()
    }
    capture_count = sum(bool(signature[11]) for signature in signatures)
    if (trigger_counts != EXPECTED_NATIVE_TRIGGER_COUNTS or
            effect_counts != EXPECTED_NATIVE_EFFECT_COUNTS or
            capture_count != EXPECTED_NATIVE_CAPTURE_RULES):
        raise RuntimeError(
            "native behavior family counts changed: "
            f"triggers={trigger_counts}, effects={effect_counts}, "
            f"captures={capture_count}")

    canonical = bytearray()
    for packed in packed_rules:
        canonical.extend(packed.to_bytes(8, "little"))
    digest = hashlib.sha256(canonical).hexdigest().upper()
    return packed_rules, resource_ranges, signatures, digest

def strip_case_suffix(value: str, suffix: str) -> tuple[str, bool]:
    if value.lower().endswith(suffix.lower()):
        return value[:-len(suffix)].rstrip(), True
    return value, False

def compile_move_input(raw_input: str, category: str, context: str,
                       actor_resource: str, owner_resource: str) -> tuple[
                           list[int] | None, int, str]:
    value = raw_input.strip()
    lowered = value.lower()
    if lowered in ("-", "automatic"):
        return None, 0, "non-input"
    if lowered == "attack while guarding":
        return None, 0, "ambiguous-input"

    flags = 0
    if value.lower().startswith("j."):
        flags |= MOVE_FLAG_AIR_NOTATION
        value = value[2:].strip()

    after_match = re.search(r"\s+after\s+", value, flags=re.IGNORECASE)
    if after_match:
        flags |= MOVE_FLAG_FOLLOW_UP | MOVE_FLAG_MODE_DEPENDENT
        value = value[:after_match.start()].rstrip()

    value, close_qualifier = strip_case_suffix(value, " (close)")
    if close_qualifier:
        flags |= MOVE_FLAG_CLOSE_QUALIFIER
    value, partner_qualifier = strip_case_suffix(value, " (partner mode)")
    if partner_qualifier:
        flags |= MOVE_FLAG_MODE_DEPENDENT

    repeated = False
    value, repeated = strip_case_suffix(value, " repeatedly")
    if repeated:
        flags |= MOVE_FLAG_REPEATED_BUTTON

    if actor_resource.upper() != owner_resource.upper():
        flags |= MOVE_FLAG_PARTNER_ACTOR | MOVE_FLAG_MODE_DEPENDENT
    if context == "follow-up state" or category == "follow-up":
        flags |= MOVE_FLAG_FOLLOW_UP | MOVE_FLAG_MODE_DEPENDENT
    if category == "partner command":
        flags |= MOVE_FLAG_MODE_DEPENDENT

    if repeated:
        if value not in LOGICAL_BUTTON:
            return None, 0, "ambiguous-repeated-input"
        button = LOGICAL_BUTTON[value]
        steps = [0]
        for _ in range(MOVE_REPEAT_TAP_COUNT):
            steps.extend((button, 0))
    else:
        direction_count = 0
        while (direction_count < len(value)
               and value[direction_count] in LOGICAL_DIRECTION):
            direction_count += 1
        directions = value[:direction_count]
        button_text = value[direction_count:]
        if button_text.startswith("+"):
            button_text = button_text[1:]
        buttons = button_text.split("+") if button_text else []
        if (not directions and not buttons
                or any(button not in LOGICAL_BUTTON for button in buttons)
                or len(set(buttons)) != len(buttons)):
            return None, 0, "ambiguous-input"

        button_mask = 0
        for button in buttons:
            button_mask |= LOGICAL_BUTTON[button]
        if len(buttons) > 1:
            flags |= MOVE_FLAG_MULTI_BUTTON

        steps = [0]
        previous_direction = None
        for index, digit in enumerate(directions):
            direction = LOGICAL_DIRECTION[digit]
            if direction != 0 and direction == previous_direction:
                steps.append(0)
            mask = direction
            if index + 1 == len(directions):
                mask |= button_mask
            steps.append(mask)
            previous_direction = direction
        if not directions:
            steps.append(button_mask)
        steps.append(0)

    if len(steps) > MOVE_MAXIMUM_SEQUENCE_STEPS:
        raise RuntimeError(
            f"{owner_resource} {raw_input!r}: compiled sequence has "
            f"{len(steps)} steps, maximum is {MOVE_MAXIMUM_SEQUENCE_STEPS}")
    if steps[0] != 0 or steps[-1] != 0:
        raise RuntimeError(f"{owner_resource} {raw_input!r}: missing neutral edge")

    if ((category in ("special", "super")
            and context != "follow-up state")
            or (category == "partner command" and value == "214+D")):
        flags |= MOVE_FLAG_GUARD_CANCEL_ELIGIBLE
    return steps, flags, ""

def classify_trigger_moves(profile: dict[str, tuple[dict, dict]]) -> tuple[
        list[tuple], list[int], list[int], list[tuple], str]:
    records: list[tuple] = []
    relative_steps: list[int] = []
    character_offsets = [0]
    excluded: list[tuple] = []

    for character_id, resource in enumerate(RESOURCES[:34]):
        document, _ = profile[resource]
        catalog = document["moveCatalog"]
        if (int(catalog["characterId"]) != character_id
                or str(catalog["resource"]).upper() != resource):
            raise RuntimeError(f"unexpected move catalog identity for {resource}")

        for move in catalog["moves"]:
            input_text = str(move["input"])
            category_text = str(move["category"])
            context_text = str(move["context"])
            actor_resource = str(move["actorResource"]).upper()
            if category_text not in MOVE_CATEGORY or context_text not in MOVE_CONTEXT:
                raise RuntimeError(
                    f"{resource} {input_text!r}: unknown category/context "
                    f"{category_text!r}/{context_text!r}")
            steps, flags, exclusion = compile_move_input(
                input_text, category_text, context_text,
                actor_resource, resource)
            action_value = move.get("actionId")
            action_id = (MOVE_UNKNOWN_ACTION if action_value is None
                         else int(action_value))
            if action_value is None:
                flags |= MOVE_FLAG_ACTION_UNKNOWN
            if not 0 <= action_id <= MOVE_UNKNOWN_ACTION:
                raise RuntimeError(f"{resource} {input_text!r}: action does not fit")

            signature = (
                character_id, action_id, input_text, str(move["name"]),
                str(move["displayLabel"]), category_text, context_text,
                actor_resource,
            )
            if steps is None:
                excluded.append(signature + (exclusion,))
                continue

            step_begin = len(relative_steps)
            relative_steps.extend(steps)
            records.append((
                character_id, action_id, step_begin, len(steps),
                MOVE_CATEGORY[category_text], MOVE_CONTEXT[context_text],
                flags, input_text, str(move["displayLabel"]), actor_resource,
            ))
        character_offsets.append(len(records))

    canonical = bytearray()
    for record in records:
        for value, width in zip(record[:7], (1, 2, 2, 1, 1, 1, 2)):
            canonical.extend(int(value).to_bytes(width, "little"))
        for value in record[7:]:
            encoded = value.encode("utf-8")
            canonical.extend(len(encoded).to_bytes(2, "little"))
            canonical.extend(encoded)
    for step in relative_steps:
        canonical.extend(step.to_bytes(4, "little"))
    maximum_per_character = max(
        end - begin
        for begin, end in zip(character_offsets, character_offsets[1:]))
    maximum_steps = max(record[3] for record in records)
    if (len(records) != EXPECTED_TRIGGER_MOVE_ROWS
            or len(relative_steps) != EXPECTED_TRIGGER_MOVE_STEPS
            or len(excluded) != EXPECTED_TRIGGER_MOVE_EXCLUDED
            or maximum_per_character
                != EXPECTED_TRIGGER_MOVE_MAXIMUM_PER_CHARACTER
            or maximum_steps != MOVE_MAXIMUM_SEQUENCE_STEPS):
        raise RuntimeError(
            "playable move catalog invariant changed: "
            f"rows={len(records)}, steps={len(relative_steps)}, "
            f"excluded={len(excluded)}, max-per-character="
            f"{maximum_per_character}, max-steps={maximum_steps}")
    digest = hashlib.sha256(canonical).hexdigest().upper()
    return records, relative_steps, character_offsets, excluded, digest

def render(table: list[list[int]], digest: str,
           native_rules: list[int], native_ranges: list[int],
           native_digest: str, move_records: list[tuple],
           move_steps: list[int], move_offsets: list[int],
           move_excluded: list[tuple], move_digest: str) -> str:
    authored = sum(bool(value & (FLAG_AUTHORED << 8))
                   for row in table for value in row)
    authored_ids = {
        action_id for action_id in range(STATE_COUNT)
        if any(row[action_id] & (FLAG_AUTHORED << 8) for row in table)
    }
    if authored != EXPECTED_AUTHORED or len(authored_ids) != EXPECTED_DISTINCT_IDS:
        raise RuntimeError(
            f"catalog invariant changed: authored={authored}, IDs={len(authored_ids)}")

    lines = [
        "// Generated by generate_training_action_catalog.py. Do not hand-edit.",
        "// Source: Attack Studio ChrLoader + analyzeAllActions for November/1126",
        "// and December/WithSE, plus NativeActionLinks.cpp behavior rules.",
        "// CHR timing fields are deliberately not represented.",
        f"// Shared structural classification SHA-256: {digest}",
        f"// Shared native behavior SHA-256: {native_digest}",
        "constexpr std::uint32_t kGeneratedResourceCount = 35u;",
        "constexpr std::uint32_t kGeneratedStateCount = 256u;",
        f"constexpr std::uint32_t kGeneratedAuthoredStateCount = {authored}u;",
        ("constexpr std::uint32_t kGeneratedDistinctAuthoredIdCount = "
         f"{len(authored_ids)}u;"),
        "constexpr bool kGeneratedProfilesClassifyEquivalently = true;",
        "constexpr std::uint32_t kTrainingActionPacked[35][256] = {",
    ]
    for resource, row in zip(RESOURCES, table):
        lines.append(f"    {{ // {resource}")
        for offset in range(0, STATE_COUNT, 8):
            values = ", ".join(f"0x{value:08X}u" for value in row[offset:offset + 8])
            lines.append(f"        {values},")
        lines.append("    },")
    lines.append("};")
    lines.extend([
        "",
        ("constexpr std::uint32_t kGeneratedNativeMoveBehaviorRuleCount = "
         f"{len(native_rules)}u;"),
        ("constexpr std::uint32_t kGeneratedNativeMoveBehaviorSourceStateCount = "
         f"{EXPECTED_NATIVE_BEHAVIOR_SOURCE_STATES}u;"),
        ("constexpr bool "
         "kGeneratedNativeMoveBehaviorProfilesClassifyEquivalently = true;"),
        ("constexpr std::uint64_t "
         f"kTrainingNativeMoveBehaviorPacked[{len(native_rules)}] = {{"),
    ])
    for offset in range(0, len(native_rules), 4):
        values = ", ".join(
            f"0x{value:016X}ULL" for value in native_rules[offset:offset + 4])
        lines.append(f"    {values},")
    lines.extend([
        "};",
        "constexpr std::uint16_t kTrainingNativeMoveBehaviorResourceRanges[35] = {",
    ])
    for offset in range(0, len(native_ranges), 8):
        values = ", ".join(
            f"0x{value:04X}u" for value in native_ranges[offset:offset + 8])
        lines.append(f"    {values},")
    lines.append("};")
    lines.extend([
        "",
        "struct GeneratedTrainingMoveRecord {",
        "    std::uint16_t actionId;",
        "    std::uint16_t stepBegin;",
        "    std::uint16_t flags;",
        "    std::uint8_t stepCount;",
        "    std::uint8_t category;",
        "    std::uint8_t context;",
        "    const char* input;",
        "    const char* label;",
        "    const char* actorResource;",
        "};",
        ("constexpr std::uint32_t kGeneratedTrainingMoveRecordCount = "
         f"{len(move_records)}u;"),
        ("constexpr std::uint32_t kGeneratedTrainingMoveStepCount = "
         f"{len(move_steps)}u;"),
        ("constexpr std::uint32_t kGeneratedTrainingMoveExcludedCount = "
         f"{len(move_excluded)}u;"),
        ("constexpr std::uint32_t kGeneratedTrainingMoveMaximumPerCharacter = "
         f"{max(b - a for a, b in zip(move_offsets, move_offsets[1:]))}u;"),
        ("constexpr std::uint32_t kGeneratedTrainingMoveMaximumSequenceSteps = "
         f"{max(record[3] for record in move_records)}u;"),
        ("constexpr char kGeneratedTrainingMoveCatalogSha256[] = "
         f"{json.dumps(move_digest)};"),
        ("constexpr GeneratedTrainingMoveRecord "
         f"kTrainingMoveRecords[{len(move_records)}] = {{"),
    ])
    for record in move_records:
        (_, action_id, step_begin, step_count, category, context, flags,
         input_text, label, actor_resource) = record
        lines.append(
            "    { "
            f"{action_id}u, {step_begin}u, 0x{flags:04X}u, "
            f"{step_count}u, {category}u, {context}u, "
            f"{json.dumps(input_text)}, {json.dumps(label)}, "
            f"{json.dumps(actor_resource)} }},")
    lines.extend([
        "};",
        ("constexpr std::uint32_t "
         f"kTrainingMoveRelativeSteps[{len(move_steps)}] = {{"),
    ])
    for offset in range(0, len(move_steps), 8):
        values = ", ".join(
            f"0x{value:05X}u" for value in move_steps[offset:offset + 8])
        lines.append(f"    {values},")
    lines.extend([
        "};",
        "constexpr std::uint16_t kTrainingMoveCharacterOffsets[35] = {",
    ])
    for offset in range(0, len(move_offsets), 8):
        values = ", ".join(
            f"{value}u" for value in move_offsets[offset:offset + 8])
        lines.append(f"    {values},")
    lines.extend(["};", ""])
    return "\n".join(lines)

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--attack-cli", required=True, type=pathlib.Path)
    parser.add_argument("--november-root", required=True, type=pathlib.Path)
    parser.add_argument("--december-root", required=True, type=pathlib.Path)
    parser.add_argument(
        "--output", type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("TrainingActionCatalogData.inc"))
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="qoh99-action-catalog-") as temp_name:
        temp = pathlib.Path(temp_name)
        november = export_profile(args.attack_cli, args.november_root, temp / "nov")
        december = export_profile(args.attack_cli, args.december_root, temp / "dec")

        nov_keys = {doc[0]["gameBuild"]["profileKey"] for doc in november.values()}
        dec_keys = {doc[0]["gameBuild"]["profileKey"] for doc in december.values()}
        if nov_keys != {PROFILE_KEYS[0]} or dec_keys != {PROFILE_KEYS[1]}:
            raise RuntimeError(f"unexpected profiles: November={nov_keys}, December={dec_keys}")

        nov_table, nov_digest = classify(november)
        dec_table, dec_digest = classify(december)
        if nov_table != dec_table or nov_digest != dec_digest:
            mismatches = [
                f"{RESOURCES[r]}:{a}" for r in range(len(RESOURCES))
                for a in range(STATE_COUNT) if nov_table[r][a] != dec_table[r][a]
            ]
            raise RuntimeError(
                "profile classification differs at " + ", ".join(mismatches[:32]))

        nov_rules, nov_ranges, nov_signatures, nov_rule_digest = \
            classify_native_behaviors(november)
        dec_rules, dec_ranges, dec_signatures, dec_rule_digest = \
            classify_native_behaviors(december)
        if (nov_rules != dec_rules or nov_ranges != dec_ranges or
                nov_signatures != dec_signatures or
                nov_rule_digest != dec_rule_digest):
            raise RuntimeError(
                "November/December NativeMoveBehavior classifications differ")

        (nov_moves, nov_move_steps, nov_move_offsets,
         nov_move_excluded, nov_move_digest) = classify_trigger_moves(november)
        (dec_moves, dec_move_steps, dec_move_offsets,
         dec_move_excluded, dec_move_digest) = classify_trigger_moves(december)
        if (nov_moves != dec_moves or nov_move_steps != dec_move_steps or
                nov_move_offsets != dec_move_offsets or
                nov_move_excluded != dec_move_excluded or
                nov_move_digest != dec_move_digest):
            raise RuntimeError(
                "November/December playable move catalogs differ")

        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            render(nov_table, nov_digest, nov_rules, nov_ranges,
                   nov_rule_digest, nov_moves, nov_move_steps,
                   nov_move_offsets, nov_move_excluded, nov_move_digest),
            encoding="utf-8", newline="\n")
        print(
            f"wrote {args.output} (actions={nov_digest}; "
            f"native={nov_rule_digest}; moves={nov_move_digest}; "
            f"moveRows={len(nov_moves)}; moveSteps={len(nov_move_steps)}; "
            f"excluded={len(nov_move_excluded)}; "
            f"maxMoves={max(b - a for a, b in zip(nov_move_offsets, nov_move_offsets[1:]))}; "
            f"maxSteps={max(record[3] for record in nov_moves)})")

if __name__ == "__main__":
    main()
