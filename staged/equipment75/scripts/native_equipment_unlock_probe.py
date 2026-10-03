"""Staged connected unlock / native custom-class equipment fixture.

Requires reviewed native equipment+audit endpoints; their absence fails. This
script never gives weapons, edits mpdata, injects GSC, or substitutes configured
class IDs for live native inventory. Full weapon/combat/objective/replay gates
remain false even when every scoped fixture assertion passes.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import time
import zlib

PROJECT = next((path for path in Path(__file__).resolve().parents if (path / "engine/src/khz").is_dir()), None)
if PROJECT is None:
    raise RuntimeError("Run the proposal beneath the cod4x1khz project")
sys.path.insert(0, str(PROJECT / "scripts"))
from native_bot_matrix_probe import MatrixFixture, supplement_provenance
from native_bot_probe import prepare_menu
from native_checkpoint_capture_probe import integer, require_no_errors, debt_bounds, leave_frontend_complete
from native_lifecycle_probe import continuity, server_continuity
from native_fov_probe import interactive_preferences, stock_inventory
from native_bot_admission_probe import confined

COUNT, STATS_BYTES, SIDECAR_BYTES, ENCODED_BYTES = 3498, 8192, 14012, 8476
TABLES = ("mp/statstable.csv", "mp/attachmenttable.csv", "mp/ranktable.csv")
PERK_NAMES = ("specialty_gpsjammer", "specialty_bulletaccuracy", "specialty_fastreload", "specialty_rof",
    "specialty_holdbreath", "specialty_bulletpenetration", "specialty_grenadepulldeath", "specialty_pistoldeath",
    "specialty_quieter", "specialty_parabolic", "specialty_longersprint", "specialty_detectexplosive",
    "specialty_explosivedamage", "specialty_exposeenemy", "specialty_bulletdamage", "specialty_extraammo",
    "specialty_twoprimaries", "specialty_armorvest", "specialty_fraggrenade", "specialty_specialgrenade")


def fnv(data):
    result = 2166136261
    for byte in data:
        result = ((result ^ byte) * 16777619) & 0xffffffff
    return result


def read_stat(data, index):
    if len(data) != STATS_BYTES or not integer(index, 0, COUNT - 1):
        raise ValueError("Raw native stat buffer/index bounds")
    return data[index + 4] if index < 2000 else struct.unpack_from("<I", data, index * 4 - 5996)[0]


def unsigned(value):
    if type(value) is not str or not re.fullmatch(r"[0-9]+", value) or int(value) > 0x7fffffff:
        raise ValueError("Actual stock table number is invalid")
    return int(value)


class Reader:
    def __init__(self, data):
        self.data, self.position = data, 0

    def raw(self, length):
        if not integer(length) or self.position + length > len(self.data):
            raise ValueError("Truncated native audit")
        value = self.data[self.position:self.position + length]
        self.position += length
        return value

    def number(self, kind="I"):
        return struct.unpack("<" + kind, self.raw(struct.calcsize("<" + kind)))[0]

    def text(self, maximum, encoding="utf-8"):
        length = self.number("H")
        if length > maximum:
            raise ValueError("Native audit text capacity")
        raw = self.raw(length)
        if b"\0" in raw:
            raise ValueError("Embedded NUL in native audit")
        return raw.decode(encoding)


def table_plan(tables):
    items, addons, ranks = (tables[name] for name in TABLES)

    def lookup(rows, column, key, result):
        found = [row[result] for row in rows if row[column] == key]
        if not found:
            raise ValueError("Required actual stock table key missing: " + key)
        return found[0]  # Matches native StringTable_Lookup first-row semantics.

    if unsigned(lookup(ranks, 0, "maxrank", 1)) != 54 or unsigned(lookup(ranks, 0, "54", 14)) != 55:
        raise ValueError("Actual rank table is not the stock level-55 contract")
    minimum = unsigned(lookup(ranks, 0, "54", 2))
    if not minimum:
        raise ValueError("Stock rank-55 XP threshold missing")
    available, counts = [], dict(weapons=0, perks=0, features=0, attachments=0, camos=0)
    for row in addons:
        group = row[2].lower()
        if group != "camo" and not group.startswith("attachment_"):
            continue
        camo = group == "camo"
        flag, mask = unsigned(row[11 if camo else 9]), unsigned(row[10])
        if flag > 7 or not 0 < mask <= 65535 or mask & (mask - 1) or not row[4]:
            raise ValueError("Actual attachment/camo flag is invalid")
        available.append((row[4], mask, camo))
        counts["camos" if camo else "attachments"] += 1
    masks, m16 = [0] * COUNT, False
    for row in items:
        reference, group = row[4], row[2].lower()
        if not reference:
            continue
        primary = group in ("weapon_smg", "weapon_assault", "weapon_sniper", "weapon_shotgun", "weapon_lmg")
        weapon = primary or group in ("weapon_pistol", "weapon_projectile", "weapon_grenade", "weapon_explosive")
        perk = group in ("specialty", "inventory", "grenade", "specialgrenade", "null_specialty")
        feature = group == "feature"
        if not (weapon or perk or feature):
            continue
        index = unsigned(row[1])
        if not 0 <= index < COUNT or not (index <= 200 or 250 <= index < 270 or 3000 <= index < 3150) or \
                (weapon and not 3000 <= index < 3150) or (perk and index >= 200) or (feature and not 250 <= index < 270):
            raise ValueError("Actual stock item points outside ownership stat ranges")
        mask = 1
        if weapon:
            counts["weapons"] += 1
            for name, addon, camo in available:
                if camo and primary or not camo and name.lower() in row[8].lower().split():
                    mask |= addon
            m16 |= reference.lower() == "m16" and index == 3025
        elif perk:
            counts["perks"] += 1
        else:
            counts["features"] += 1
            if reference.lower() == "feature_cac":
                masks[200] |= 1
        masks[index] |= mask
    if not m16 or any(counts[name] < minimum for name, minimum in
            (("weapons", 25), ("perks", 15), ("features", 5), ("attachments", 5), ("camos", 6))):
        raise ValueError("Actual captured stock unlock tables are incomplete")
    return masks, minimum, counts


def parse_audit(data):
    if type(data) is not bytes or not 50000 <= len(data) <= 524288:
        raise ValueError("Native completed audit byte capacity")
    reader = Reader(data)
    if reader.raw(8) != b"KHZUA075" or reader.number() != 1:
        raise ValueError("Unknown native completed-audit schema")
    serial = reader.number("Q")
    count, raw_bytes, minimum, before_crc, candidate_crc, sidecar_bytes, encoded_bytes, table_bytes = [reader.number() for _ in range(8)]
    if not serial or (count, raw_bytes, sidecar_bytes, encoded_bytes) != (COUNT, STATS_BYTES, SIDECAR_BYTES, ENCODED_BYTES) or not 4 <= table_bytes <= 393216:
        raise ValueError("Native audit dimensions/serial do not match the real transaction")
    home, profile, backup = reader.text(1023), reader.text(95), reader.text(95)
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,95}", profile) or profile in (".", "..") or not re.fullmatch(r"[A-Za-z0-9_.-]{1,95}", backup) or backup in (".", ".."):
        raise ValueError("Native audit profile/backup path token")
    before, candidate = reader.raw(STATS_BYTES), reader.raw(STATS_BYTES)
    masks = [reader.number() for _ in range(COUNT)]
    sidecar, encoded = reader.raw(SIDECAR_BYTES), reader.raw(ENCODED_BYTES)
    table_blob = reader.raw(table_bytes)
    if reader.position != len(data) or encoded[:4] != b"iwm0":
        raise ValueError("Native audit has trailing bytes or wrong actual encrypted-file header")
    if zlib.crc32(before[4:]) != before_crc or zlib.crc32(candidate[4:]) != candidate_crc or struct.unpack_from("<I", candidate)[0] != candidate_crc:
        raise ValueError("Independent candidate/raw audit payload checksum differs")
    table_reader = Reader(table_blob)
    if table_reader.number() != 3:
        raise ValueError("Audit did not capture every actual stock table")
    tables = {}
    for expected, required in zip(TABLES, (11, 12, 15)):
        name, rows, columns = table_reader.text(95), table_reader.number(), table_reader.number()
        if name != expected or name in tables or not 2 <= rows <= 1024 or not required <= columns <= 32:
            raise ValueError("Actual stock table identity/dimensions differ")
        tables[name] = [[table_reader.text(1023, "latin1") for _ in range(columns)] for _ in range(rows)]
    if table_reader.position != len(table_blob):
        raise ValueError("Actual stock table capture has trailing/unparsed bytes")
    actual_masks, actual_minimum, counts = table_plan(tables)
    if actual_masks != masks or actual_minimum != minimum:
        raise ValueError("Captured masks/XP do not follow the actual captured stock table contract")
    magic, version, saved_count, saved_minimum, checksum = struct.unpack_from("<5I", sidecar)
    if (magic, version, saved_count, saved_minimum) != (0x3155484b, 1, COUNT, minimum) or list(struct.unpack_from("<3498I", sidecar, 20)) != masks or fnv(sidecar[:16] + b"\0" * 4 + sidecar[20:]) != checksum:
        raise ValueError("Persisted unlock record integrity/masks/stock XP differ")
    changed = []
    for index, mask in enumerate(masks):
        old, now = read_stat(before, index), read_stat(candidate, index)
        expected = max(old, minimum) if index == 2301 else old | mask
        if now != expected or (index != 2301 and (now & ~mask) != (old & ~mask)):
            raise ValueError(f"Actual unlock changed an unaffected stat or unmasked bit at {index}")
        if now != old:
            changed.append(index)
    if candidate[7996:] != before[7996:]:
        raise ValueError("Unlock changed an unnamed trailing native payload byte")
    return dict(serial=serial, home=home, profile=profile, backup=backup, before=before, candidate=candidate,
                masks=masks, minimum_xp=minimum, tables=tables, sidecar=sidecar, encoded=encoded,
                stats_compared=COUNT, every_unmasked_bit_preserved=True, changed_indices=changed,
                actual_table_counts=counts, before_header_checksum_valid=struct.unpack_from("<I", before)[0] == before_crc,
                candidate_checksum_valid=True, encoded_sha256=hashlib.sha256(encoded).hexdigest())


def player_errors(player, source, slot):
    errors = []
    if type(player) is not dict or player.get("source") != source or player.get("client_num") != slot:
        return ["Actual player-state source/client identity differs"]
    count = player.get("weapon_catalogue_count")
    if not integer(count, 1, 128) or any(not integer(player.get(name), 0, count - 1) for name in ("weapon", "offhand")):
        errors.append("Current/offhand index exceeds the actual native catalogue")
    for name, length, low, high in (("owned", 4, 0, 0xffffffff), ("ammo", 128, -2147483648, 2147483647),
            ("clips", 128, -2147483648, 2147483647), ("models", 128, 0, 255)):
        values = player.get(name)
        if type(values) is not list or len(values) != length or any(not integer(value, low, high) for value in values):
            errors.append("Actual native array malformed: " + name)
    if errors:
        return errors
    owned = {index for index in range(128) if player["owned"][index // 32] & (1 << (index % 32))}
    required = owned | {player["weapon"], player["offhand"]}
    required.discard(0)
    if 0 in owned or any(index >= count for index in owned):
        errors.append("Ownership bits exceed the actual weapon catalogue")
    weapons = player.get("weapons")
    if type(weapons) is not list or not 1 <= len(weapons) <= 16 or any(type(value) is not dict or not integer(value.get("index"), 1, count - 1) for value in weapons):
        return errors + ["Actual bounded native weapon records missing"]
    if len({value["index"] for value in weapons}) != len(weapons) or {value["index"] for value in weapons} != required:
        errors.append("Native weapon metadata does not cover exactly owned/current/offhand indices")
    for weapon in weapons:
        index = weapon["index"]
        if weapon.get("owned") is not (index in owned) or not integer(weapon.get("model"), 0, 15) or weapon["model"] != player["models"][index]:
            errors.append("Native owned/model fields mismatch or escape the sixteen model entries")
        for name in ("ammo_index", "clip_index"):
            if not integer(weapon.get(name), 0, 127):
                errors.append("Native ammo/clip pool bound: " + name)
        if not integer(weapon.get("alternate"), 0, count - 1):
            errors.append("Native alternate index outside actual catalogue")
        for name in ("name", "gun_model", "world_model"):
            value = weapon.get(name)
            if value is None and name != "name":
                continue
            if type(value) is not str or not value or len(value.encode("utf-8")) >= 192 or "\0" in value:
                errors.append("Actual bounded asset name missing: " + name)
    if not integer(player.get("perks"), 0, (1 << 20) - 1) or not integer(player.get("command_time"), -2147483648, 2147483647):
        errors.append("Native perks or command clock malformed")
    return errors


def equipment_errors(server, client, ready):
    sample, inspection = ready["sample"], ready["inspection"]
    owner = sample["stats"]["client_owner"]
    humans = [value for value in inspection["slots"] if value.get("bot") is False]
    if len(humans) != 1:
        return ["Exactly one actual admitted human required"]
    human = humans[0]
    errors = []
    for value in (server, client):
        if value.get("schema") != 1 or value.get("complete") is not True or value.get("reason") != "none" or any(value.get(name) is not False for name in ("full_weapon_acceptance", "objective_acceptance", "full_world_replay_verified")):
            errors.append("Native equipment missing/failed or falsely claimed complete gameplay acceptance")
        if value.get("session") != inspection["session"] or value.get("server_id") != inspection["server_id"]:
            errors.append("Equipment was captured in a different native session/serverId")
    if server.get("process_id") != ready["server"]["process_id"] or server.get("source") != "dedicated_authority" or server.get("generation_namespace") != "human_admission" or server.get("human_generation") != human["generation"] or server.get("slot") != human["slot"]:
        errors.append("Server equipment does not attest the actual human admission namespace")
    if server.get("received_stats_packet_mask") != 127 or server.get("session_state") != 0 or not integer(server.get("health"), 1) or not integer(server.get("last_spawn_time"), 1):
        errors.append("Live human received-stats mask/spawn gate failed")
    if client.get("process_id") != sample["stats"]["process_id"] or client.get("source") != "acknowledged_private_owner_barrier" or client.get("generation_namespace") != "client_owner" or client.get("owner_generation") != owner["generation"] or not integer(client.get("owner_tick"), 1):
        errors.append("Client equipment is not actual acknowledged private-owner state")
    if any(not integer(client.get(name), 1) for name in ("accepted_current_message", "accepted_next_message")) or client["accepted_current_message"] > client["accepted_next_message"]:
        errors.append("Actual accepted gameplay snapshot frontier missing")
    for key, source in (("player", "authoritative_native_ps"),):
        errors += player_errors(server.get(key), source, human["slot"])
    errors += player_errors(client.get("accepted"), "accepted_gameplay_next_snapshot", human["slot"])
    errors += player_errors(client.get("predicted"), "actual_private_predicted_ps", human["slot"])
    return errors


def current_profile(fixture):
    active = confined(fixture.home / "players/profiles/active.txt", fixture.project)
    name = active.read_text(encoding="ascii").strip()
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,95}", name) or name in (".", ".."):
        raise ValueError("Actual isolated active-profile token is invalid")
    return confined(active.parent / name, fixture.home)


def retain_file(fixture, reply, expected_name, destination, maximum):
    if reply.get("schema") != 1 or reply.get("committed") is not True or reply.get("reason") != "none" or reply.get("file") != expected_name or not integer(reply.get("bytes"), 1, maximum) or not re.fullmatch(r"[0-9a-f]{64}", reply.get("sha256", "")):
        raise ValueError("Native immutable file export was not completed")
    source = confined(current_profile(fixture) / expected_name, fixture.home)
    with source.open("rb") as file:
        data = file.read(maximum + 1)
    if len(data) != reply["bytes"] or hashlib.sha256(data).hexdigest() != reply["sha256"]:
        raise ValueError("Actual committed native file checksum/length differs")
    (fixture.output / destination).write_bytes(data)
    return data


def equipment(fixture, ready):
    sample, inspection = ready["sample"], ready["inspection"]
    human = next(value for value in inspection["slots"] if value["bot"] is False)
    server = fixture.rcon(f"khz_equipment_inspect {human['slot']} {inspection['session']} {human['generation']}", "KHZ_EQUIPMENT_INSPECTION")
    owner = sample["stats"]["client_owner"]
    client = fixture.call(f"khz_equipment {owner['session']} {owner['generation']}", "KHZ_EQUIPMENT_CLIENT")
    require_no_errors(equipment_errors(server, client, ready))
    return {"server": server, "client": client}


def human_identity(ready):
    human = next(value for value in ready["inspection"]["slots"] if value["bot"] is False)
    owner = ready["sample"]["stats"]["client_owner"]
    return {"slot": human["slot"], "human_generation": human["generation"], "owner_generation": owner["generation"],
            "session": ready["inspection"]["session"], "server_id": ready["inspection"]["server_id"]}


def wait_reconnect(fixture, old, name):
    stage = {"name": name, "passed": False, "attempts": []}
    fixture.report.setdefault("reconnects", []).append(stage)
    until = min(fixture.deadline, time.monotonic() + 30)
    previous = human_identity(old)
    while time.monotonic() < until:
        ready = fixture.wait_case(name, min(5, max(.1, until - time.monotonic())))
        current = human_identity(ready)
        stage["attempts"].append(current)
        if any(current[key] != previous[key] for key in ("session", "server_id")):
            raise ValueError("Normal reconnect changed the unchanged native server map/session")
        if current["human_generation"] > previous["human_generation"] and current["owner_generation"] > previous["owner_generation"]:
            later = fixture.wait_case(name + " fresh accepted controls", 5)
            if human_identity(later) != current:
                raise ValueError("New human admission changed before reconnect recovery")
            stage["client_continuity"] = continuity(ready["sample"], later["sample"])
            stage["server_continuity"] = server_continuity(ready["server"], later["server"], fixture.port)
            stage["client_debt"] = debt_bounds([ready["sample"]["stats"], later["sample"]["stats"]])
            stage["server_debt"] = debt_bounds([ready["server"], later["server"]])
            stage.update(passed=True, recovered=later)
            return later
        time.sleep(.1)
    raise TimeoutError("No newer actual human/private-owner admission after native reconnect")


def choose(fixture, menu, text=None, name=None, index=None):
    if fixture.call("khz_ui menu " + menu).get("result") != "opened":
        raise ValueError("Actual compiled native menu missing: " + menu)
    items = stock_inventory(fixture, menu)
    matches = [value for value in items if value.get("type") == 1 and value.get("visible") is True and value.get("enabled") is True and value.get("action") is True and
        (text is None or value["text"] == text) and (name is None or value["name"] == name) and (index is None or value["index"] == index)]
    if len(matches) != 1:
        raise ValueError(f"Actual enabled compiled action ambiguous/missing: {menu}/{text}/{name}/{index}")
    item = matches[0]
    details = fixture.call(f"khz_ui details {menu} {item['index']}")
    if details.get("result") != "stock item details" or not details.get("action"):
        raise ValueError("Actual stock action details unavailable")
    entry = {"menu": menu, "item": item, "details": details}
    fixture.report.setdefault("stock_actions", []).append(entry)
    entry["reply"] = fixture.call(f"khz_ui activate {menu} {item['index']}")
    if entry["reply"].get("result") != "stock action executed":
        raise ValueError("Actual native item action was not executed")
    return fixture.call("khz_ui status")["top_menu"]


def select_custom_class(fixture):
    # Every selector is the actual compiled action. No stat command/write path.
    choose(fixture, "main_text", text="@MPUI_CREATE_A_CLASS")
    slots = [value for value in stock_inventory(fixture, "pc_cac_popup") if value.get("type") == 1 and value.get("visible") and value.get("enabled") and value.get("action") and value.get("text")]
    if len(slots) != 5:
        raise ValueError("Actual stock custom-class slots are missing/ambiguous")
    choose(fixture, "pc_cac_popup", index=slots[0]["index"])
    menu = choose(fixture, "menu_cac_assault", name="primary_weapon_selection")
    menu = choose(fixture, menu, name="assault")
    menu = choose(fixture, menu, text="@WEAPON_M4_CARBINE")
    candidates = [value for value in stock_inventory(fixture, menu) if value.get("type") == 1 and value.get("visible") and value.get("enabled") and value.get("action") and value.get("text") in ("@MPUI_REFLEX", "@MPUI_RED_DOT_SIGHT", "@MPUI_SILENCER", "@MPUI_ACOG_SCOPE")]
    if not candidates:
        raise ValueError("M4 has no actual supported non-GL/grip attachment action")
    menu = choose(fixture, menu, index=candidates[0]["index"])
    choose(fixture, menu, text="@MPUI_DIGITAL")
    for control, value in (("@MPUI_PERK_1", "@PERKS_BANDOLIER"), ("@MPUI_PERK_2", "@PERKS_STOPPING_POWER"), ("@MPUI_PERK_3", "@PERKS_STEADY_AIM")):
        menu = choose(fixture, "menu_cac_assault", text=control)
        choose(fixture, menu, text=value)
    menu = choose(fixture, "menu_cac_assault", text="@MPUI_SIDE_ARM1")
    menu = choose(fixture, menu, text="@WEAPON_DESERTEAGLEGOLD")
    if "_attachment_popup_" in menu:
        choose(fixture, menu, text="@MPUI_NO_ATTACHMENT")
    elif menu != "menu_cac_assault":
        raise ValueError("Native sidearm action opened an unsupported selector")
    menu = choose(fixture, "menu_cac_assault", text="@MPUI_SPECIAL_GRENADE")
    # Choose by actual live reference after activation; the menu's display token
    # is retained and validated, never used as a native weapon index.
    options = [value for value in stock_inventory(fixture, menu) if value.get("type") == 1 and value.get("visible") and value.get("enabled") and value.get("action") and "CONCUSSION" in value.get("text", "")]
    if len(options) != 1:
        raise ValueError("Actual enabled concussion-grenade selector unavailable")
    choose(fixture, menu, index=options[0]["index"])
    selected = fixture.call("khz_ui status")["profile"]["classes"][0]
    if selected.get("primary") != "m4" or not integer(selected.get("attachment_id"), 1, 7) or selected.get("attachment") in ("grip", "gl") or selected.get("camo_id") == 0:
        raise ValueError("Native compiled actions did not save the distinctive compatible primary/attachment/camo")
    return selected


def expected_class(selected, tables):
    rows, addons = tables[TABLES[0]], tables[TABLES[1]]
    def row_for(table, column, key):
        values = [row for row in table if row[column] == str(key)]
        if not values:
            raise ValueError("Selected real class lacks an actual stock table row")
        return values[0]
    primary, secondary = row_for(rows, 0, selected["primary_id"]), row_for(rows, 0, selected["secondary_id"])
    attach, second_attach = row_for(addons, 9, selected["attachment_id"]), row_for(addons, 9, selected["secondary_attachment_id"])
    camo = row_for(addons, 11, selected["camo_id"])
    if attach[4] not in primary[8].split() or secondary[2] != "weapon_pistol" or camo[2] != "camo":
        raise ValueError("Actual saved class violates stock weapon/attachment/sidearm/camo restrictions")
    perks = [row_for(rows, 0, index) for index in selected["perk_ids"]]
    names = [row[6] for row in perks if row[2] == "specialty"]
    if names != ["specialty_extraammo", "specialty_bulletdamage", "specialty_bulletaccuracy"]:
        raise ValueError("Actual selected stock perk references differ from the compatible fixture")
    grenade = row_for(rows, 0, selected["special_grenade_id"])
    def weapon(row, attachment, flag):
        return row[4] + ("_" + attachment[4] if flag else "") + "_mp"
    return {"primary": weapon(primary, attach, selected["attachment_id"]), "secondary": weapon(secondary, second_attach, selected["secondary_attachment_id"]),
            "grenade": grenade[4], "camo": selected["camo_id"], "perk_names": set(names)}


def loadout_errors(evidence, expected):
    server, client = evidence["server"], evidence["client"]
    errors = []
    for player in (server["player"], client["accepted"], client["predicted"]):
        inventory = {value["name"]: value for value in player["weapons"] if value["owned"]}
        for name in (expected["primary"], expected["secondary"], expected["grenade"]):
            if name not in inventory:
                errors.append("Actual native inventory lacks selected stock reference: " + name)
        if errors:
            continue
        primary = inventory[expected["primary"]]
        if player["weapon"] != primary["index"] or primary["model"] != expected["camo"] or not primary["gun_model"] or not primary["world_model"]:
            errors.append("Actual primary/current/camo models do not match the saved stock selection")
        for name in (expected["primary"], expected["secondary"]):
            value = inventory[name]
            if player["ammo"][value["ammo_index"]] <= 0 or player["clips"][value["clip_index"]] <= 0:
                errors.append("Actual spawned stock firearm has no native reserve/clip ammo: " + name)
        value = inventory[expected["grenade"]]
        if player["clips"][value["clip_index"]] <= 0:
            errors.append("Actual selected special grenade has no native clip count")
        actual_perks = {PERK_NAMES[bit] for bit in range(20) if player["perks"] & (1 << bit)}
        if actual_perks != expected["perk_names"]:
            errors.append("Actual authoritative/accepted/predicted perks disagree with captured stock class rows")
    return errors


def run(fixture):
    fixture.launch_shell()
    prepare_menu(fixture, "settings_quick_war", 0, 1)
    if fixture.call("khz_ui host mp_crash war 0 1").get("result") != "start requested":
        raise ValueError("Actual stock zero-bot TDM host refused")
    before = fixture.wait_case("locked active native human")
    if before["sample"]["ui"]["profile"].get("all_unlocks") is not False:
        raise ValueError("Fresh isolated fixture unexpectedly already unlocked")
    fixture.report["baseline_equipment"] = equipment(fixture, before)
    backups = current_profile(fixture) / "khz_backups"
    original_backups = set(backups.iterdir()) if backups.exists() else set()
    fixture.report["unlock_request"] = fixture.call("khz_ui unlockall")
    if fixture.report["unlock_request"].get("result") != "queued":
        raise ValueError("Connected native unlock action refused")
    unlocked = fixture.wait_ready_unlock = wait_reconnect(fixture, before, "connected unlock refresh")
    status = fixture.call("khz_unlock_audit status", "KHZ_UNLOCK_AUDIT")
    if status.get("complete") is not True or not integer(status.get("serial"), 1):
        raise ValueError("No actual completed successful native unlock audit")
    exported = fixture.call("khz_unlock_audit export " + str(status["serial"]), "KHZ_UNLOCK_AUDIT", timeout=5)
    raw = retain_file(fixture, exported, "khz_unlock_audit.bin", "completed-unlock-audit.bin", 524288)
    audit = parse_audit(raw)
    profile = current_profile(fixture)
    if audit["serial"] != status["serial"] or Path(audit["home"]).resolve() != fixture.home.resolve() or audit["profile"] != profile.name:
        raise ValueError("Completed audit differs from this actual isolated active profile")
    backup = confined(profile / "khz_backups" / audit["backup"], fixture.home)
    created = set(backups.iterdir()) - original_backups
    if created != {backup} or (backup / "memory_stats.bin").read_bytes() != audit["before"] or f"memory_stats_fnv1a={fnv(audit['before'])}\n" not in (backup / "backup.txt").read_text():
        raise ValueError("Actual transaction did not produce exactly one complete matching backup")
    if (profile / "khz_unlocks.bin").read_bytes() != audit["sidecar"] or (profile / "mpdata").read_bytes() != audit["encoded"]:
        raise ValueError("Actual persisted sidecar/encrypted file differ from successful immutable transaction")
    fixture.report["unlock_audit"] = {key: value for key, value in audit.items() if key not in ("before", "candidate", "masks", "tables", "sidecar", "encoded")}
    fixture.report["unlock_audit_export"] = exported
    fixture.restore_backup = audit["backup"]
    fixture.report["unlocked_equipment"] = equipment(fixture, unlocked)
    identity = human_identity(unlocked)
    before_sidecar = hashlib.sha256((profile / "khz_unlocks.bin").read_bytes()).hexdigest()
    fixture.report["idempotent_request"] = fixture.call("khz_ui unlockall")
    time.sleep(.15)
    unchanged = fixture.wait_case("idempotent connected unlock")
    repeated = fixture.call("khz_unlock_audit status", "KHZ_UNLOCK_AUDIT")
    if fixture.report["idempotent_request"].get("result") != "queued" or human_identity(unchanged) != identity or repeated != status or set(backups.iterdir()) != original_backups | created or hashlib.sha256((profile / "khz_unlocks.bin").read_bytes()).hexdigest() != before_sidecar:
        raise ValueError("Idempotent unlock changed the actual admission/audit/backups/entitlement")
    fixture.report["idempotent_completed_commit"] = True
    selected = fixture.report["selected_native_class"] = select_custom_class(fixture)
    expected = expected_class(selected, audit["tables"])
    fixture.report["expected_from_actual_stock_tables"] = {**expected, "perk_names": sorted(expected["perk_names"])}
    snapshot_reply = fixture.call("khz_profile_snapshot", "KHZ_PROFILE_SNAPSHOT", timeout=5)
    saved_class = retain_file(fixture, snapshot_reply, "khz_profile_snapshot.bin", "selected-live-profile.bin", STATS_BYTES)
    request = fixture.report["class_reconnect_request"] = fixture.call("khz_ui console reconnect")
    if request.get("result") != "queued":
        raise ValueError("Reviewed exact native reconnect test allowlist is unavailable")
    refreshed = wait_reconnect(fixture, unchanged, "saved custom class stats refresh")
    initial_equipment = equipment(fixture, refreshed)
    if initial_equipment["server"]["received_class_stats_201_249"] != [read_stat(saved_class, index) for index in range(201, 250)]:
        raise ValueError("Dedicated authority did not receive the actual saved class bytes")
    team = initial_equipment["server"]["team"]
    menu = {1: "changeclass_opfor", 2: "changeclass_marines"}.get(team)
    if not menu:
        raise ValueError("Actual TDM native team has no reviewed stock class menu")
    if fixture.call("khz_ui menu " + menu).get("result") != "opened":
        raise ValueError("Actual loaded in-game stock class menu is unavailable")
    candidates = []
    for item in stock_inventory(fixture, menu):
        if item.get("type") == 1 and item.get("visible") and item.get("enabled") and item.get("action"):
            details = fixture.call(f"khz_ui details {menu} {item['index']}")
            if re.search(r'\bscriptmenuresponse\s+"?custom1(?:,0)?"?(?:\s|;|$)', details.get("action", ""), re.I):
                candidates.append((item, details))
    if len(candidates) != 1:
        raise ValueError("Actual enabled compiled custom1 scriptmenuresponse action missing or ambiguous")
    fixture.report["compiled_custom1_response"] = {"menu": menu, "item": candidates[0][0], "details": candidates[0][1]}
    choose(fixture, menu, index=candidates[0][0]["index"])
    # Stock menuClass can defer gear outside grace. The only proposed writer is
    # an exact gated native kill command, preserving its real death/respawn path.
    request = fixture.report["native_kill_request"] = fixture.call("khz_ui console kill")
    if request.get("result") != "queued":
        raise ValueError("Validated stock kill/respawn fixture seam is unavailable")
    old_spawn = initial_equipment["server"]["last_spawn_time"]
    until = min(fixture.deadline, time.monotonic() + 15)
    death = None
    fixture.report["respawn_samples"] = samples = []
    while time.monotonic() < until:
        inspected = fixture.rcon("khz_bot_inspect", "KHZ_BOT_INSPECTION")
        human = next((value for value in inspected.get("slots", []) if value.get("bot") is False), None)
        samples.append(inspected)
        if human is None or human.get("generation") != human_identity(refreshed)["human_generation"]:
            raise ValueError("Native kill/respawn changed the validated human admission")
        if human.get("session_state") == 2 and integer(human.get("health"), -2147483648, 0) and human.get("last_spawn_time") == old_spawn:
            death = inspected
        if death and human.get("session_state") == 0 and integer(human.get("health"), 1) and human.get("last_spawn_time", 0) > old_spawn:
            recovered = fixture.wait_case("actual custom1 native respawn")
            final = equipment(fixture, recovered)
            require_no_errors(loadout_errors(final, expected))
            fixture.report["live_custom1_equipment"] = final
            fixture.report["actual_death_then_new_live_spawn"] = True
            fixture.report["death_observation"] = death
            break
    else:
        raise TimeoutError("Actual stock death and newer live custom-class spawn not observed")
    fixture.leave()
    restore(fixture, audit["before"])
    fixture.report["passed"] = True


def restore(fixture, expected=None):
    if not getattr(fixture, "restore_backup", None):
        return
    current = fixture.sample()
    if not leave_frontend_complete(current["stats"], current["state"], current["ui"], fixture.server is None or not fixture.server.alive()):
        raise ValueError("Restore requires actual native Leave/disconnected stock frontend")
    reply = fixture.call("khz_ui console restoreunlocks " + fixture.restore_backup)
    if reply.get("result") != "queued":
        raise ValueError("Actual native profile restore was not queued")
    time.sleep(.1)
    snapshot = fixture.call("khz_profile_snapshot", "KHZ_PROFILE_SNAPSHOT", timeout=5)
    memory = retain_file(fixture, snapshot, "khz_profile_snapshot.bin", "restored-live-profile.bin", STATS_BYTES)
    if expected is None:
        expected = (current_profile(fixture) / "khz_backups" / fixture.restore_backup / "memory_stats.bin").read_bytes()
    if memory[4:] != expected[4:] or fixture.call("khz_ui status")["profile"].get("all_unlocks") is not False:
        raise ValueError("Native restore differs from every original stat/unlocked status")
    fixture.report["every_restored_payload_byte_matches"] = True
    fixture.restore_backup = None


def self_test():
    player = {"source": "actual", "client_num": 0, "command_time": 1, "weapon": 1, "offhand": 2,
              "weapon_catalogue_count": 3, "owned": [6, 0, 0, 0], "ammo": [10] * 128, "clips": [10] * 128,
              "models": [0] * 128, "perks": 1,
              "weapons": [{"index": index, "owned": True, "model": 0, "alternate": 0, "ammo_index": 0,
                           "clip_index": 0, "name": "actual" + str(index), "gun_model": None, "world_model": None} for index in (1, 2)]}
    assert not player_errors(player, "actual", 0)
    for field, value in (("models", [255] * 128), ("weapon", 128), ("weapon_catalogue_count", True), ("perks", 1 << 20), ("owned", [6, 0, 0, 1 << 31])):
        assert player_errors({**player, field: value}, "actual", 0)
    assert player_errors({**player, "weapons": player["weapons"][:1]}, "actual", 0)
    for bad in (b"", b"KHZUA075", b"x" * 524289, b"\0" * 50000):
        try:
            parse_audit(bad)
        except ValueError:
            pass
        else:
            raise AssertionError("Malformed/truncated native audit accepted")
    before = bytes(range(256)) * 32
    assert read_stat(before, 1999) == before[2003] and read_stat(before, 2000) == struct.unpack_from("<I", before, 2004)[0]
    assert fnv(b"hello") == 0x4f9f2cab
    print("Equipment source/array/model guards and strict native audit parsing selftests pass; no live gameplay proof")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-archive", type=Path)
    parser.add_argument("--client-port", type=int, default=28991)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--seconds", type=int, default=120)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test(); return 0
    if os.name != "nt" or args.build_archive is None or not 1024 <= args.client_port <= 65535 or not 90 <= args.seconds <= 150:
        parser.error("Explicit frozen archive, Windows, port1024..65535 and bounded90..150s are required")
    archive = confined(args.build_archive.absolute(), PROJECT)
    output = confined((args.output or PROJECT / "results" / ("native-equipment-unlock-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f"))).absolute(), PROJECT / "results")
    if output.exists() or output.parent != PROJECT / "results":
        parser.error("Use a new direct child of project/results")
    output.mkdir()
    fixture = MatrixFixture(PROJECT, output, args.client_port,
        {"name": "human_connected_custom1", "map": "mp_crash", "gametype": "war", "bot_count": 0, "bot_difficulty": 1}, args.seconds)
    fixture.report.update(equipment_probe_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        full_weapon_acceptance=False, objective_acceptance=False, full_world_replay_verified=False,
        scope="Completed real connected unlock/stat backup audit and actual dedicated/accepted/predicted custom-class inventory; firing/reload/projectile/damage and full-world gates remain pending")
    before = preferences = None
    try:
        before = fixture.report["provenance_before"] = supplement_provenance(PROJECT, archive)
        preferences = fixture.report["interactive_preferences_before"] = interactive_preferences(PROJECT)
        run(fixture)
    except Exception as error:
        fixture.report["errors"].append(str(error)); fixture.report["passed"] = False
    finally:
        if getattr(fixture, "restore_backup", None) and fixture.client and fixture.client.poll() is None:
            try:
                if fixture.server and fixture.server.alive():
                    fixture.leave()
                restore(fixture)
            except Exception as error:
                fixture.report["errors"].append("Native isolated-profile restoration: " + str(error))
        fixture.close()
        try:
            fixture.report["provenance_after_cleanup"] = after = supplement_provenance(PROJECT, archive)
            fixture.report["provenance_unchanged"] = before is not None and before == after
            fixture.report["interactive_preferences_after"] = after_preferences = interactive_preferences(PROJECT)
            fixture.report["interactive_preferences_unchanged"] = preferences is not None and preferences == after_preferences
            if not fixture.report["provenance_unchanged"] or not fixture.report["interactive_preferences_unchanged"]:
                fixture.report["errors"].append("Frozen source/binaries/assets or interactive preferences changed")
        except Exception as error:
            fixture.report["errors"].append("Provenance verification: " + str(error))
        fixture.report["passed"] = fixture.report["passed"] and not fixture.report["errors"]
        (output / "result.json").write_text(json.dumps(fixture.report, indent=2), encoding="utf-8")
    print(json.dumps({"passed": fixture.report["passed"], "errors": fixture.report["errors"], "output": str(output / "result.json")}))
    return 0 if fixture.report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
