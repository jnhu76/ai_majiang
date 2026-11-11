"""Validate ADR-0001's documentation fixtures offline (requires jsonschema 4.x)."""

import argparse
from copy import deepcopy
import json
from pathlib import Path
import re
import sys

from jsonschema import Draft202012Validator, FormatChecker, ValidationError

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / "examples"
# Synthetic CharacterPack identities. The JSON examples contain BehaviorPacks,
# not CharacterPack manifests or installed voice assets.
PROFILES = {
    "org.aimj.demo.jiang-xiaokuai": ("jiang-xiaokuai", "0.9.2", "jj-bright"),
    "org.aimj.demo.wen-amo": ("wen-amo", "1.0.0", "amo-soft"),
}
SLOTS_BY_TRIGGER = {
    "domain_event": {"claim_count", "tile"},
    "hand_phase": {"streak"},
}
TOKEN = re.compile(r"\{([a-z_]+)\}")
TILES = [number + suit for suit in "万筒条" for number in "一二三四五六七八九"]
TILES += ["东风", "南风", "西风", "北风", "红中", "发财", "白板"]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def no_duplicate_keys(pairs):
    result = dict(pairs)
    require(len(result) == len(pairs), "duplicate JSON member")
    return result


def reject_constant(value):
    raise ValueError(f"invalid JSON constant {value}")


def read_json(path):
    with path.open(encoding="utf-8") as source:
        return json.load(source, object_pairs_hook=no_duplicate_keys, parse_constant=reject_constant)


def index(rows, field):
    result = {row[field]: row for row in rows}
    require(len(result) == len(rows), f"duplicate {field}")
    return result


def bind_clip(asset, text, voice, clips, hashes):
    signature = (voice, text)  # zh-Hans is the only locale in this schema.
    clip = asset["clip"]
    digest = asset["audio_sha256"]
    require(clips.setdefault(clip, (digest, signature)) == (digest, signature),
            f"conflicting clip binding: {clip}")
    require(hashes.setdefault(digest, signature) == signature,
            f"audio hash reused with different text or voice: {digest}")


def validate_line(line, clips, hashes):
    text = line["text"]
    slots = {slot["name"] for slot in line["slots"]}
    require(set(TOKEN.findall(text)) == slots and not re.search(r"[{}]", TOKEN.sub("", text)),
            f"{line['id']}: placeholders do not match declared slots")
    voice = line["voice"]
    audio = voice.get("audio", {})
    if audio.get("mode") == "clip":
        bind_clip(audio, text, voice["voice_id"], clips, hashes)
    if audio.get("mode") == "variants":
        seen = set()
        for variant in audio["variants"]:
            bindings = variant["bindings"]
            require(bindings.keys() == slots, f"{line['id']}: incomplete or extra variant bindings")
            key = tuple(sorted(bindings.items()))
            require(key not in seen, f"{line['id']}: duplicate variant binding")
            seen.add(key)
            rendered = TOKEN.sub(
                lambda token: TILES[bindings["tile"]] if token[1] == "tile" else str(bindings[token[1]]),
                text,
            )
            require(variant["text"] == rendered, f"{line['id']}: variant text does not match bindings")
            bind_clip(variant, rendered, voice["voice_id"], clips, hashes)


def validate_guards(guards, kind):
    for guard in guards:
        query = guard.get("query")
        if query == "hand_claim_count_by_seat_min":
            require(kind == "domain_event", "claim count requires a meld event")
        if query in {"last_hand_winner", "win_streak_min"}:
            require(kind == "hand_phase", f"{query} requires first_draw")
        if query == "relationship_affinity_max":
            validate_role(guard["params"]["toward"], kind)


def validate_role(role, kind):
    require(role != "event_target" or kind == "domain_event", "trigger has no event_target")


def validate_examples(schema, catalog, packs, proposal):
    for example, kind in (
        (catalog, "scenarioCatalog"), *((pack, "behaviorPack") for pack in packs),
        (proposal, "proposalMetadata"),
    ):
        root = {key: schema[key] for key in ("$schema", "$defs")}
        root["$ref"] = f"#/$defs/{kind}"
        Draft202012Validator(root, format_checker=FormatChecker()).validate(example)

    scenes = index(catalog["scenarios"], "scenario_id")
    pack_by_id = index(packs, "pack_id")
    eligible_root_acts = set()
    audio_hashes = {}
    for scene in scenes.values():
        validate_guards(scene.get("guards", ()), scene["trigger"]["kind"])

    for pack in packs:
        minimum = tuple(map(int, pack["catalog"]["min_revision"].split(".")))
        revision = tuple(map(int, catalog["catalog_revision"].split(".")))
        require(minimum <= revision, f"{pack['pack_id']}: incompatible catalog revision")
        character = pack["character_ref"]
        profile = PROFILES.get(character["pack_id"])
        require(profile is not None and character["character_id"] == profile[0],
                f"{pack['pack_id']}: unknown character_ref in fixtures")
        lines = index(pack["lines"], "id")
        clips = {}
        for line in lines.values():
            require(line["voice"]["voice_id"] == profile[2], f"{line['id']}: unknown voice_id in fixture")
            validate_line(line, clips, audio_hashes)
        reaction_ids = set()
        for scene_id, entry in index(pack["scenarios"], "scenario_id").items():
            require(scene_id in scenes, f"unknown scenario_id: {scene_id}")
            scene = scenes[scene_id]
            kind = scene["trigger"]["kind"]
            reactions = index(entry["reactions"], "reaction_id")
            require(reaction_ids.isdisjoint(reactions), "duplicate reaction_id in pack")
            reaction_ids.update(reactions)
            for position, reaction in enumerate(reactions.values(), 1):
                match = reaction["match"]
                unconditional = (
                    match.get("observer_scope", "any") == "any"
                    and not match.get("trait_bands") and not match.get("guards")
                )
                require(not unconditional or position == len(reactions),
                        f"{reaction['reaction_id']}: catch-all shadows later reactions")
                if match.get("observer_scope") == "event_target":
                    validate_role("event_target", kind)
                for lower, upper in match.get("trait_bands", {}).values():
                    require(lower <= upper, f"{reaction['reaction_id']}: inverted trait band")
                validate_guards(match.get("guards", ()), kind)
                if "relationship_rule" in reaction:
                    validate_role(reaction["relationship_rule"]["toward"], kind)
                if reaction["delivery"] == "speak" and scene["max_followups"] == 1:
                    eligible_root_acts.add(reaction["dialogue_act"])
                for line_id in reaction.get("lines", ()):
                    require(line_id in lines, f"{reaction['reaction_id']}: unknown line_id {line_id}")
                    line = lines[line_id]
                    require(line["dialogue_act"] == reaction["dialogue_act"],
                            f"{line_id}: dialogue_act differs from reaction")
                    require(all(slot["name"] in SLOTS_BY_TRIGGER.get(kind, ()) for slot in line["slots"]),
                            f"{line_id}: slot unavailable for {kind}")

    for scene in scenes.values():
        trigger = scene["trigger"]
        if trigger["kind"] == "exchange_followup":
            require(trigger["requires_act"] in eligible_root_acts,
                    f"{scene['scenario_id']}: no eligible root dialogue_act")

    require(proposal["scenario_id"] in scenes, "proposal: unknown scenario_id in fixtures")
    require(proposal["source_event_schema"] == f"scenario-catalog.v1@{catalog['catalog_revision']}",
            "proposal: unknown catalog revision in fixtures")
    pack_id, version = proposal["target_pack"].split("@")
    require(pack_id in pack_by_id and version == pack_by_id[pack_id]["version"],
            "proposal: unknown target_pack revision in fixtures")
    pack = pack_by_id[pack_id]
    character_pack_id = pack["character_ref"]["pack_id"]
    require(proposal["character_revision"] == f"{character_pack_id}@{PROFILES[character_pack_id][1]}",
            "proposal: unknown character_revision in fixtures")
    require(proposal["inputs"]["fixture_id"] == "golden.idle", "proposal: unknown fixture_id")
    require(set(proposal["inputs"].get("recent_delivered_sample", ())) <= {line["id"] for line in pack["lines"]},
            "proposal: unknown recent line ID")
    require(proposal["teacher_role"] == proposal["proposal_id"][11:].rsplit("-", 1)[0],
            "proposal: role differs from proposal_id")


def self_test(schema, examples):
    catalog, (jiang, wen), _ = examples
    first_line = jiang["lines"][0]
    dynamic = jiang["lines"][3]
    target = wen["scenarios"][2]["reactions"][1]["match"]
    followup = catalog["scenarios"][1]
    claim = catalog["scenarios"][2]
    claim_reaction = jiang["scenarios"][1]["reactions"][0]
    clip = {"mode": "clip", "clip": "fixture-clip", "audio_sha256": "a" * 64}
    finite = {"mode": "variants", "variants": [
        {"bindings": {"claim_count": 2}, "text": dynamic["text"].replace("{claim_count}", "2"),
         "clip": "fixture-claim-2", "audio_sha256": "b" * 64}
    ]}
    first_line["voice"]["audio"] = clip
    dynamic["voice"]["audio"] = finite
    validate_examples(schema, *examples)
    print("PASS 2 additional audio shapes (static clip and finite variant metadata)")

    cases = [
        ("required", first_line, "text", None, None),
        ("unknown field", first_line, "typo", True, None),
        ("field type", first_line, "weight", "2", None),
        ("weight range", first_line, "weight", 0, None),
        ("observer scope", target, "observer_scope", "discarder", None),
        ("nonterminal response", followup, "max_followups", 1, None),
        ("system cue as domain event", claim["trigger"], "event_type", "backend_unavailable", None),
        ("unknown trigger", claim["trigger"], "kind", "custom_trigger", None),
        ("unknown guard", claim_reaction["match"]["guards"][0], "kind", "custom_guard", None),
        ("guard parameter range", claim_reaction["match"]["guards"][0]["params"], "min", 0, None),
        ("neutral delta", wen["scenarios"][2]["reactions"][1]["emotion_rule"], "intensity_delta", 0.1, None),
        ("invalid audio hash", clip, "audio_sha256", "placeholder", None),
        ("dynamic fixed clip", dynamic["voice"], "audio", clip, None),
        ("wrong variant transcript", finite["variants"][0], "text", "第3次", "variant text"),
        ("wrong binding set", finite["variants"][0], "bindings", {"streak": 2}, "variant bindings"),
        ("duplicate variant", finite, "variants", finite["variants"] * 2, "duplicate variant"),
        ("missing slot", dynamic, "text", "缺少槽位", "placeholders"),
        ("wrong dialogue act", dynamic, "dialogue_act", "praise", "dialogue_act"),
        ("bad line ref", claim_reaction, "lines", ["missing_line"], "unknown line_id"),
        ("duplicate line ID", first_line, "id", jiang["lines"][1]["id"], "duplicate id"),
        ("bad scenario ref", jiang["scenarios"][0], "scenario_id", "unknown.scene", "unknown scenario_id"),
        ("bad voice ref", first_line["voice"], "voice_id", "missing-voice", "unknown voice_id"),
        ("unknown character", jiang["character_ref"], "character_id", "missing-character", "unknown character_ref"),
        ("unreachable followup", followup["trigger"], "requires_act", "question", "no eligible root"),
        ("reversed trait band", claim_reaction["match"]["trait_bands"], "competitiveness", [1, 0], "inverted trait band"),
        ("shadowed match", claim_reaction, "match", {}, "catch-all"),
    ]
    for label, obj, key, replacement, diagnostic in cases:
        original = obj.pop(key, None)
        if replacement is not None:
            obj[key] = replacement
        try:
            validate_examples(schema, *examples)
        except ValidationError:
            require(diagnostic is None, f"{label}: expected semantic rejection")
        except ValueError as error:
            require(diagnostic is not None and diagnostic in str(error), f"{label}: unexpected rejection: {error}")
        else:
            raise ValueError(f"{label}: invalid fixture was accepted")
        finally:
            obj.pop(key, None)
            if original is not None:
                obj[key] = original
    print(f"PASS {len(cases)} negative controls (Schema and cross-reference rules)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="also check positive and negative controls")
    args = parser.parse_args()
    schema = read_json(HERE / "npc-v1.schema.json")
    Draft202012Validator.check_schema(schema)
    catalog = read_json(FIXTURES / "scenario-catalog.v1.json")
    packs = [read_json(FIXTURES / f"behavior-pack-{name}.v1.json") for name in ("jiang-xiaokuai", "wen-amo")]
    proposal = read_json(FIXTURES / "proposal-metadata.v1.json")
    validate_examples(schema, catalog, packs, proposal)
    print("PASS 4 example files (Draft 2020-12 and fixture semantics)")
    if args.self_test:
        self_test(schema, deepcopy((catalog, packs, proposal)))


if __name__ == "__main__":
    try:
        main()
    except ValidationError as error:
        sys.exit(f"FAIL {error.json_path}: {error.message}")
    except ValueError as error:
        sys.exit(f"FAIL: {error}")
