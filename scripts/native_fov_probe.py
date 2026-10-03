"""Check actual rendered hip/ADS geometry and sensitivity in an isolated match.

Uses the native benchmark input and stock local hosting. This is scoped FOV
evidence, not full movement, profile, public-client or replay equivalence.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import shlex
import time

from native_checkpoint_capture_probe import ManagedHostFixture
from native_lifecycle_probe import continuity, ready_errors, server_continuity


OPTIONS_CATEGORIES = ("main_options", "options_graphics", "options_graphics_texture", "options_sound", "options_voice",
                      "options_game", "options_multi", "options_look", "options_move", "options_shoot", "options_misc", "controls_multi")
K_ENTER, K_LEFT, K_RIGHT = 13, 156, 157  # Native ui/keycodes.h.


def degrees_from_vertical_tangent(state):
    value = state.get("tan_half_fov_y")
    if type(value) not in (float, int) or not math.isfinite(value) or value <= 0:
        raise ValueError("Actual rendered FOV tangent is invalid")
    return math.degrees(2 * math.atan(value / .75))


def expected_geometry(hip, ads, scale):
    hip = max(10., min(160., hip * scale))
    ads = max(10., min(160., ads * scale))
    return ads, math.tan(math.radians(ads / 2)) / math.tan(math.radians(hip / 2))


def wait_view(fixture, scale, ads, weapon=None, timeout=8):
    until = min(fixture.deadline, time.monotonic() + timeout)
    last = None
    while time.monotonic() < until:
        sample = fixture.sample()
        state = sample["state"]
        fraction = state.get("predicted_weapon_pos_frac")
        if (state.get("connection") == 9 and state.get("weapon", 0) > 0 and
                state.get("pm_type") == 0 and not state.get("other_flags", 0) & 2 and
                (weapon is None or state["weapon"] == weapon) and
                type(fraction) in (int, float) and abs(fraction - int(ads)) < .00001 and
                abs(state.get("fov_scale", -1) - scale) < .00001):
            fov = degrees_from_vertical_tangent(state)
            if last and abs(fov - last["fov_degrees"]) < .003:
                return {"sample": sample, "fov_degrees": fov}
            last = {"sample": sample, "fov_degrees": fov}
        else:
            last = None
        time.sleep(.04)
    raise TimeoutError(f"Actual live rendered {'ADS' if ads else 'hip'} view did not settle at scale {scale}")


def console(fixture, text):
    reply = fixture.call("khz_ui console " + text)
    if reply.get("result") != "queued":
        raise RuntimeError("Native console action rejected: " + text)


def run(fixture, extended=False):
    fixture.start()
    report = fixture.report
    report["scope"] = "Actual rendered hip/ADS FOV and sensitivity with native one-ms input, stock weapon, and non-cheat console alias. Same-map capture/replay and complete weapon coverage are separate gates."
    report["cases"] = []
    console(fixture, "khz_test_ads 0")
    hip = wait_view(fixture, 1., False)
    weapon = hip["sample"]["state"]["weapon"]
    console(fixture, "khz_test_ads 1")
    ads = wait_view(fixture, 1., True, weapon)
    base_hip, base_ads = hip["fov_degrees"], ads["fov_degrees"]
    if not abs(base_hip - 65) < .02 or not 10 <= base_ads < base_hip:
        raise RuntimeError("Stock unscaled hip/ADS rendered geometry does not match the fixture")
    report["unscaled"] = {"hip": hip, "ads": ads, "weapon": weapon}
    for scale in (.2, .5, 1.25, 2.):
        console(fixture, f"fovscale {scale}")
        for aimed in (False, True):
            console(fixture, "khz_test_ads " + str(int(aimed)))
            actual = wait_view(fixture, scale, aimed, weapon)
            expected_fov, expected_sensitivity = expected_geometry(base_hip, base_ads if aimed else base_hip, scale)
            state = actual["sample"]["state"]
            passed = (abs(actual["fov_degrees"] - expected_fov) < .025 and
                      abs(state["zoom_sensitivity"] - expected_sensitivity) < .002)
            report["cases"].append({"name": f"Native {'ADS' if aimed else 'hip'} geometry and sensitivity at {scale}",
                                    "passed": passed, "expected_fov": expected_fov,
                                    "expected_sensitivity": expected_sensitivity, "actual": actual})
            if not passed:
                raise RuntimeError(report["cases"][-1]["name"] + " failed")
    # Invalid alias values must preserve the current archived bounded value.
    console(fixture, "fovscale nan")
    retained = wait_view(fixture, 2., True, weapon)
    report["cases"].append({"name": "Nonfinite alias input preserves current scale", "passed": True, "actual": retained})
    console(fixture, "khz_test_ads 0")
    console(fixture, "fovscale 1")
    restored = wait_view(fixture, 1., False, weapon)
    report["cases"].append({"name": "Original hip geometry restored", "passed": abs(restored["fov_degrees"] - base_hip) < .025,
                            "actual": restored})
    if extended:
        run_extended(fixture, base_hip, base_ads, weapon)
    fixture.leave()
    report["passed"] = all(case["passed"] for case in report["cases"])


def stock_inventory(fixture, menu):
    items, offset, total = [], 0, None
    while True:
        reply = fixture.call(f"khz_ui inspect {menu} {offset}")
        validate_inventory_page(reply, menu, offset, total)
        total = reply["item_count"]
        items.extend(reply["items"])
        offset += 16
        if offset >= reply["item_count"]:
            return items


def validate_inventory_page(reply, menu, offset, total=None):
    if (reply.get("result") != "inspected" or reply.get("menu") != menu or reply.get("offset") != offset or
            type(reply.get("item_count")) is not int or not 0 <= reply["item_count"] <= 512 or
            (total is not None and reply["item_count"] != total) or type(reply.get("items")) is not list or
            len(reply["items"]) != min(16, max(0, reply["item_count"] - offset))):
        raise ValueError("Actual stock menu inventory is unavailable, changed, or truncated: " + menu)
    for index, item in enumerate(reply["items"], offset):
        if (type(item) is not dict or type(item.get("index")) is not int or item["index"] != index or
                any(type(item.get(name)) is not bool for name in ("visible", "enabled", "action")) or
                type(item.get("type")) is not int or any(type(item.get(name)) is not str for name in ("name", "text", "dvar"))):
            raise ValueError("Actual stock menu item metadata is malformed: " + menu)


def one_control(items, *, dvar=None, text=None, name=None, kind=None, require_action=True):
    matches = [item for item in items if item.get("visible") is True and item.get("enabled") is True and (not require_action or item.get("action") is True)
               and (dvar is None or item.get("dvar", "").casefold() == dvar.casefold()) and (text is None or item.get("text") == text)
               and (name is None or item.get("name") == name) and (kind is None or item.get("type") == kind)]
    if len(matches) != 1:
        raise ValueError(f"Expected one actual enabled stock control {dvar or text or name}; found {len(matches)}")
    return matches[0]


def stock_action(fixture, menu, item, key=None):
    details = fixture.call(f"khz_ui details {menu} {item['index']}")
    if details.get("result") != "stock item details":
        raise ValueError("Native compiled stock action is unavailable")
    command = f"khz_ui activate {menu} {item['index']}" if key is None else f"khz_ui key {menu} {item['index']} {key}"
    reply = fixture.call(command)
    proof = {"item": item, "compiled_details": details, "command": command, "reply": reply}
    fixture.report.setdefault("stock_action_attempts", []).append(proof)
    expected = "stock action executed" if key is None else "stock key dispatched"
    if reply.get("result") != expected:
        raise ValueError("Actual enabled stock action rejected: " + command)
    if key is not None and reply.get("handled") is False:
        raise ValueError("Native selected stock widget did not handle the actual key: " + command)
    return proof


def select_restart_confirmation(controls, command):
    # Use actual compiled button scripts rather than assumed item indexes or
    # localized captions. A cancel button merely naming the popup is excluded.
    expression = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(command) + r"(?![A-Za-z0-9_])", re.IGNORECASE)
    matches = [control for control in controls if control["item"].get("visible") is True and
               control["item"].get("enabled") is True and control["item"].get("action") is True and
               control["item"].get("type") == 1 and expression.search(control["details"].get("action", ""))]
    if len(matches) != 1:
        raise ValueError(f"Expected one actual enabled stock confirmation dispatching {command}; found {len(matches)}")
    return matches[0]


def confirm_stock_apply(fixture, menu, category):
    command = "vid_restart" if menu == "options_graphics" else "snd_restart"
    allowed = ("vid_restart_popmenu", "vid_restart_popmenu_listen") if command == "vid_restart" else ("snd_restart_popmenu", "vid_restart_popmenu_listen")
    proof = category["confirmation"] = {"expected_command": command, "popup_samples": [], "passed": False}
    until = min(fixture.deadline, time.monotonic() + 2)
    popup = None
    while time.monotonic() < until:
        status = fixture.call("khz_ui status")
        proof["popup_samples"].append(status)
        if status.get("top_menu") in allowed:
            popup = status["top_menu"]
            break
        time.sleep(.04)
    if popup is None:
        raise TimeoutError("Actual stock Apply did not open its restart confirmation dialog")
    proof["menu"] = popup
    items = proof["items"] = stock_inventory(fixture, popup)
    controls = proof["controls"] = []
    for item in items:
        if item.get("type") == 1 and item.get("visible") is True and item.get("enabled") is True and item.get("action") is True:
            details = fixture.call(f"khz_ui details {popup} {item['index']}")
            if details.get("result") != "stock item details":
                raise ValueError("Native restart confirmation script is unavailable")
            controls.append({"item": item, "details": details})
            if len(controls) > 16:
                raise ValueError("Native restart popup exceeds the bounded confirmation-control scope")
    selected = select_restart_confirmation(controls, command)
    proof["selected"] = selected
    proof["action"] = stock_action(fixture, popup, selected["item"])
    proof["passed"] = True
    return proof


def archived_config(fixture):
    profiles = fixture.home / "players/profiles"
    active = (profiles / "active.txt").read_text(encoding="latin1").strip()
    profile = profiles / active
    if not active or len(active) > 64 or profile.resolve().parent != profiles.resolve() or profile.is_symlink() or profile.is_junction():
        raise ValueError("Native active benchmark profile is not a confined profile child")
    path = profile / "config_mp.cfg"
    if path.is_symlink() or path.stat().st_size > 256 * 1024:
        raise ValueError("Native archived benchmark config is not a bounded regular file")
    data = path.read_bytes()
    settings, bindings = parse_archived_config(data)
    return {"path": str(path), "sha256": hashlib.sha256(data).hexdigest(), "settings": settings, "bindings": bindings}


def parse_archived_config(data):
    if type(data) is not bytes or len(data) > 256 * 1024 or b"\0" in data:
        raise ValueError("Archived config is not bounded native text")
    settings, bindings = {}, {}
    for line in data.decode("latin1").splitlines():
        if line.startswith("seta ") or line.startswith("bind "):
            tokens = shlex.split(line)
            if len(tokens) != 3:
                raise ValueError("Archived setting/binding line has an invalid token count")
            (settings if tokens[0] == "seta" else bindings)[tokens[1]] = tokens[2]
    return settings, bindings


def archive_evidence(value, settings=(), bindings=()):
    return {"path": value["path"], "sha256": value["sha256"],
            "settings": {name: value["settings"].get(name) for name in settings},
            "bindings": {name: value["bindings"].get(name) for name in bindings}}


def wait_archived(fixture, name, *, previous=None, expected=None, binding=False, timeout=3):
    until = min(fixture.deadline, time.monotonic() + timeout)
    while time.monotonic() < until:
        value = archived_config(fixture)
        actual = value["bindings" if binding else "settings"].get(name)
        if actual is not None and ((expected is not None and actual == expected) or
                                   (expected is None and actual != previous)):
            return value
        time.sleep(.05)
    raise TimeoutError(f"Native archived {'binding' if binding else 'setting'} {name} did not record the actual stock action")


def wait_recovery(fixture, name, map_name, before, *, restart=False, changed_map=False, timeout=30):
    stage = {"name": name, "passed": False, "samples": [], "server_samples": []}
    fixture.report.setdefault("extended_recovery", []).append(stage)
    until = min(fixture.deadline, time.monotonic() + timeout)
    first = first_server = None
    while time.monotonic() < until:
        if not fixture.server.alive():
            raise RuntimeError("The exact owned server exited during FOV/Options recovery")
        try:
            current = fixture.sample(); stage["samples"].append(current)
            errors = ready_errors(current, map_name, fixture.client.pid, fixture.server.pid)
            old, new = before["stats"]["client_owner"], current["stats"].get("client_owner", {})
            if restart and not new.get("generation", 0) > old["generation"]:
                errors.append("The actual Options Apply did not produce a new gameplay owner generation")
            if changed_map and new.get("session") == old["session"] and current["state"].get("server_id") == before["state"].get("server_id"):
                errors.append("The actual map action did not change the native map/session lifetime")
            stage["last_errors"] = errors
            if not errors:
                if first is None:
                    first, first_server = current, fixture.native_server()
                    stage["server_samples"].append(first_server)
                elif current["elapsed"] - first["elapsed"] >= .3:
                    stage["continuity"] = continuity(first, current)
                    final_server = fixture.native_server(); stage["server_samples"].append(final_server)
                    stage["server_continuity"] = server_continuity(first_server, final_server, fixture.port)
                    stage.update(passed=True, recovered=current)
                    return current
            else:
                first = first_server = None
        except (TimeoutError, OSError, ValueError) as error:
            stage["last_query_error"] = str(error); first = first_server = None
        time.sleep(.05)
    raise TimeoutError(f"Native {name} did not recover: {stage.get('last_errors')}; {stage.get('last_query_error')}")


def verify_scaled_view(fixture, name, scale, base_hip, base_ads, weapon):
    values = []
    for aimed in (False, True):
        console(fixture, "khz_test_ads " + str(int(aimed)))
        actual = wait_view(fixture, scale, aimed, weapon)
        fov, sensitivity = expected_geometry(base_hip, base_ads if aimed else base_hip, scale)
        passed = abs(actual["fov_degrees"] - fov) < .025 and abs(actual["sample"]["state"]["zoom_sensitivity"] - sensitivity) < .002
        values.append({"ads": aimed, "expected_fov": fov, "expected_sensitivity": sensitivity, "actual": actual, "passed": passed})
    if not all(value["passed"] for value in values):
        raise ValueError(name + " actual scaled hip/ADS projection or sensitivity changed")
    config = wait_archived(fixture, "cg_fovScale", expected=str(scale))
    fixture.report["cases"].append({"name": name, "passed": True, "views": values,
                                    "archive": archive_evidence(config, ("cg_fovScale",))})


def assert_persisted_scale(state, config, scale):
    actual = state.get("fov_scale")
    if (type(actual) not in (int, float) or not math.isfinite(actual) or abs(actual - scale) > .00001 or
            config["settings"].get("cg_fovScale") != str(scale)):
        raise ValueError("The actual live and archived FOV selection did not persist across the native lifetime transition")


def recalibrate_live_weapon(fixture, name, scale, base_hip):
    # Stock class selection can choose a different weapon after a map/respawn.
    # First prove the user selection survived untouched; only then temporarily
    # measure that real weapon's unscaled ADS optics and restore the selection.
    before, config = fixture.sample(), archived_config(fixture)
    proof = {"name": name, "before_any_recalibration": before,
             "archive_before_any_recalibration": archive_evidence(config, ("cg_fovScale",)), "passed": False}
    fixture.report.setdefault("weapon_recalibrations", []).append(proof)
    assert_persisted_scale(before["state"], config, scale)
    weapon = before["state"].get("weapon")
    if type(weapon) is not int or weapon <= 0:
        raise ValueError("The current live stock weapon is unavailable for actual ADS recalibration")
    console(fixture, "khz_test_ads 0")
    scaled_hip = wait_view(fixture, scale, False, weapon)
    expected_hip, _ = expected_geometry(base_hip, base_hip, scale)
    if abs(scaled_hip["fov_degrees"] - expected_hip) >= .025:
        raise ValueError("Scaled hip projection did not persist before actual weapon ADS recalibration")
    console(fixture, "fovscale 1")
    hip = wait_view(fixture, 1., False, weapon)
    console(fixture, "khz_test_ads 1")
    ads = wait_view(fixture, 1., True, weapon)
    if abs(hip["fov_degrees"] - base_hip) >= .025 or not 10 <= ads["fov_degrees"] < hip["fov_degrees"]:
        raise ValueError("Actual current stock weapon unscaled optics are inconsistent")
    console(fixture, "fovscale " + str(scale))
    proof.update(passed=True, weapon=weapon, scaled_hip_before_recalibration=scaled_hip,
                 unscaled_hip=hip, unscaled_ads=ads, temporary_unscaled_measurement=True)
    return hip["fov_degrees"], ads["fov_degrees"], weapon


def wait_natural_respawn(fixture, scale, timeout=25):
    stage = fixture.report["natural_respawn"] = {"passed": False, "source": "actual natural gameplay death and respawn", "samples": []}
    until = min(fixture.deadline, time.monotonic() + timeout)
    dead = None
    while time.monotonic() < until:
        current = fixture.sample()
        server = fixture.native_server()
        peers = [peer for peer in server.get("peers", []) if peer.get("port") == fixture.port]
        if len(peers) != 1:
            raise ValueError("The actual human admission is missing during natural respawn")
        peer, state = peers[0], current["state"]
        if type(peer.get("health")) is not int or type(peer.get("client")) is not int:
            raise ValueError("Actual native human health/admission fields are missing")
        stage["samples"].append({"client": current, "server": server})
        if abs(state.get("fov_scale", -1) - scale) > .00001:
            raise ValueError("Archived FOV scale changed during natural death/respawn")
        # MP pmtype_t PM_DEAD/PM_DEAD_LINKED are7/8; independent server
        # health supplies authoritative death evidence rather than a view-only
        # spectator transition. A later live native view must be this human.
        if peer["health"] <= 0 and (state.get("pm_type") in (7, 8) or state.get("other_flags", 0) & 2):
            dead = {"client": current, "server": server, "peer": peer}; stage["death"] = dead
        elif dead and peer.get("client") == dead["peer"]["client"] and peer.get("health", 0) > 0 and \
                state.get("pm_type") == 0 and not state.get("other_flags", 0) & 2 and state.get("view_client") == peer["client"]:
            stage["respawn"] = {"client": current, "server": server, "peer": peer}
            stage["passed"] = True
            return current
        time.sleep(.05)
    raise TimeoutError("No actual authoritative natural death→live human respawn was observed within the bounded FOV fixture")


def exercise_stock_category(fixture, report, menu, map_name, scale, category):
    choices = {"options_graphics_texture": ("r_texFilterMipMode", "r_texFilterMipMode"),
               "options_voice": ("cl_voice", "cl_voice"), "options_game": ("cg_blood", "cg_blood"),
               "options_multi": ("cl_allowDownload", "cl_allowDownload"), "options_look": ("m_filter", "m_filter")}
    opened = fixture.call("khz_ui menu " + menu)
    category["open"] = opened
    if opened.get("result") != "opened" or opened.get("top_menu") != menu:
        raise ValueError("Actual stock Options category did not open: " + menu)
    items = category["items"] = stock_inventory(fixture, menu)
    if menu in ("options_graphics", "options_sound"):
        before = fixture.sample()
        setting = "r_vsync" if menu == "options_graphics" else "snd_khz"
        ui_setting = "ui_r_vsync" if menu == "options_graphics" else "ui_snd_khz"
        archived = archived_config(fixture)
        if setting not in archived["settings"]:
            raise ValueError("The actual archived native Options value is missing: " + setting)
        category["before_archive"] = archive_evidence(archived, (setting, "cg_fovScale"))
        category["actions"].append(stock_action(fixture, menu, one_control(items, dvar=ui_setting, require_action=False), K_RIGHT))
        apply = one_control(stock_inventory(fixture, menu), text="@MENU_APPLY", kind=1)
        applied = stock_action(fixture, menu, apply)
        category["actions"].append(applied)
        confirmation = confirm_stock_apply(fixture, menu, category)
        recovered = wait_recovery(fixture, "stock " + menu + " confirmed Apply", map_name, before, restart=True, timeout=15)
        changed = wait_archived(fixture, setting, previous=archived["settings"][setting])
        if changed["settings"].get("cg_fovScale") != str(scale):
            raise ValueError("Stock Options Apply failed to persist the archived FOV scale")
        proof = {"category": menu, "action": applied, "confirmation": confirmation, "before": before, "recovered": recovered,
                 "after_archive": archive_evidence(changed, (setting, "cg_fovScale")), "passed": True}
        if menu == "options_graphics" and recovered["ui"].get("vsync") != (changed["settings"][setting] == "1"):
            raise ValueError("Actual post-Apply VSync does not match the archived user selection")
        report["apply"].append(proof)
    elif menu in choices:
        ui_setting, setting = choices[menu]
        archived = archived_config(fixture)
        if setting not in archived["settings"]:
            raise ValueError("The actual native setting is absent from the benchmark config: " + setting)
        category["actions"].append(stock_action(fixture, menu, one_control(items, dvar=ui_setting, require_action=False), K_RIGHT))
        changed = wait_archived(fixture, setting, previous=archived["settings"][setting])
        category["changed_archive"] = archive_evidence(changed, (setting, "cg_fovScale"))
    elif menu in ("options_move", "options_shoot", "options_misc", "controls_multi"):
        controls = [item for item in items if item.get("type") == 14 and item.get("enabled") is True and item.get("visible") is True]
        if not controls:
            raise ValueError("Actual native keybinding control is unavailable: " + menu)
        item = controls[0]
        category["actions"].append(stock_action(fixture, menu, item, K_ENTER))
        category["actions"].append(stock_action(fixture, menu, item, K_LEFT))
        changed = wait_archived(fixture, "LEFTARROW", expected=item["dvar"], binding=True)
        category["changed_archive"] = archive_evidence(changed, ("cg_fovScale",), ("LEFTARROW",))
    # Every category also follows an enabled stock navigation action.
    category["actions"].append(stock_action(fixture, menu, one_control(stock_inventory(fixture, menu), name="back", kind=1)))
    category["passed"] = True


def collect_independent_categories(names, reports, callback):
    for name in names:
        category = reports[name] = {"passed": False, "actions": []}
        try:
            callback(name, category)
        except Exception as error:
            category["error"] = str(error)
            category["passed"] = False
    return all(category["passed"] for category in reports.values()) and len(reports) == len(names)


def exercise_stock_options(fixture, map_name, scale):
    report = fixture.report["stock_options"] = {"passed": False, "categories": {}, "apply": []}
    listing = fixture.call("khz_ui list")
    report["native_menu_listing"] = listing

    def category(name, proof):
        if name not in listing.get("menus", []):
            raise ValueError("An actual stock Options category is missing: " + name)
        exercise_stock_category(fixture, report, name, map_name, scale, proof)

    # A restart failure may consume its bounded timeout; exercise the other ten
    # actual categories first so that it cannot hide their independent results.
    order = tuple(name for name in OPTIONS_CATEGORIES if name not in ("options_graphics", "options_sound")) + ("options_graphics", "options_sound")
    report["attempt_order"] = order
    categories_pass = collect_independent_categories(order, report["categories"], category)
    report["passed"] = categories_pass and len(report["apply"]) == 2
    if not report["passed"]:
        failed = [name for name, proof in report["categories"].items() if not proof["passed"]]
        fixture.report["cases"].append({"name": "Every stock Options category and real graphics/audio Apply with persisted selection",
                                         "passed": False, "failed_categories": failed,
                                         "successful_apply_count": len(report["apply"])})
    return report["passed"]


def run_extended(fixture, base_hip, base_ads, weapon):
    report = fixture.report
    report["extended"] = True
    scale = 1.25
    console(fixture, "fovscale " + str(scale))
    verify_scaled_view(fixture, "Archived FOV selection before map change", scale, base_hip, base_ads, weapon)
    before = fixture.sample()
    report["crash_map_request"] = fixture.call("khz_ui server map mp_crash")
    if report["crash_map_request"].get("result") != "server command queued":
        raise ValueError("The actual owned Crash map command was rejected")
    wait_recovery(fixture, "Crash with retained owned server", "mp_crash", before, changed_map=True)
    base_hip, base_ads, weapon = recalibrate_live_weapon(fixture, "Crash selection persistence and actual current weapon optics", scale, base_hip)
    verify_scaled_view(fixture, "Archived FOV survives actual Crash map transition", scale, base_hip, base_ads, weapon)
    try:
        wait_natural_respawn(fixture, scale)
        base_hip, base_ads, weapon = recalibrate_live_weapon(fixture, "Respawn selection persistence and actual current weapon optics", scale, base_hip)
        verify_scaled_view(fixture, "Archived FOV survives actual human death and respawn", scale, base_hip, base_ads, weapon)
    except (TimeoutError, ValueError) as error:
        # A short natural-death observer can miss a brief death. Preserve its
        # failed coverage gate and still exercise independent stock Options.
        report["cases"].append({"name": "Actual human death/respawn FOV persistence", "passed": False, "error": str(error)})
    options_pass = exercise_stock_options(fixture, "mp_crash", scale)
    current = fixture.sample()
    wait_recovery(fixture, "post-stock-Options gameplay", "mp_crash", current)
    base_hip, base_ads, weapon = recalibrate_live_weapon(fixture, "Options selection persistence and actual current weapon optics", scale, base_hip)
    if options_pass:
        verify_scaled_view(fixture, "Archived FOV survives both actual stock Options Apply restarts", scale, base_hip, base_ads, weapon)
    console(fixture, "khz_test_ads 0")
    console(fixture, "fovscale 1")
    wait_view(fixture, 1., False, weapon)
    if options_pass:
        report["cases"].append({"name": "Every stock Options category and real graphics/audio Apply with persisted selection", "passed": True})


def interactive_preferences(project):
    home = project / "runtime/interactive/players/profiles"
    if not home.exists():
        return {}
    paths = list(home.glob("*/config_mp.cfg")) + ([home / "active.txt"] if (home / "active.txt").is_file() else [])
    result = {}
    for path in paths:
        if path.is_symlink() or path.parent.is_symlink() or path.parent.is_junction() or path.resolve().is_relative_to(home.resolve()) is not True:
            raise ValueError("Interactive preference snapshot is not confined to the project profile directory")
        if path.stat().st_size > 256 * 1024:
            raise ValueError("Interactive preference snapshot exceeded native config bounds")
        result[str(path.relative_to(project))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-port", type=int, default=28992)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--extended", action="store_true", help="Also require actual Crash map, natural death/respawn, every stock Options category and graphics/audio Apply persistence")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        assert expected_geometry(65, 40, .2)[0] == 10
        assert expected_geometry(65, 65, 2) == (130, 1)
        assert abs(degrees_from_vertical_tangent({"tan_half_fov_y": math.tan(math.radians(32.5)) * .75}) - 65) < 1e-10
        for value in (0., float("nan"), float("inf"), -1, True, "0.1", None):
            try:
                degrees_from_vertical_tangent({"tan_half_fov_y": value})
            except ValueError:
                pass
            else:
                raise AssertionError("Invalid native tangent accepted")
        settings, bindings = parse_archived_config(b'// native archive\nseta cg_fovScale "1.25"\nseta r_vsync "1"\nbind LEFTARROW "+moveleft"\nbind x "say two words"\n')
        assert settings == {"cg_fovScale": "1.25", "r_vsync": "1"}
        assert bindings == {"LEFTARROW": "+moveleft", "x": "say two words"}
        assert parse_archived_config(b'seta cg_fovScale "1"\nseta cg_fovScale "1.25"\n')[0]["cg_fovScale"] == "1.25"
        for invalid in (b'seta cg_fovScale "', b'bind x a b', b'seta cg_fovScale "1"\0', b'x' * (256 * 1024 + 1)):
            try:
                parse_archived_config(invalid)
            except ValueError:
                pass
            else:
                raise AssertionError("Invalid archived setting/binding metadata accepted")
        item = {"index": 0, "visible": True, "enabled": True, "action": True, "type": 1, "name": "apply", "text": "@MENU_APPLY", "dvar": ""}
        page = {"result": "inspected", "menu": "options_graphics", "offset": 0, "item_count": 1, "items": [item]}
        validate_inventory_page(page, "options_graphics", 0)
        assert one_control([item], text="@MENU_APPLY", kind=1) is item
        assert one_control([item, dict(item, visible=False)], text="@MENU_APPLY") is item
        native_dvar_item = dict(item, dvar="cl_allowdownload", action=False)
        assert one_control([native_dvar_item], dvar="cl_allowDownload", require_action=False) is native_dvar_item
        for invalid in (dict(page, items=[]), dict(page, menu="options_sound"), dict(page, offset=16),
                        dict(page, items=[dict(item, index=1)]), dict(page, items=[dict(item, enabled=1)]), dict(page, item_count=True)):
            try:
                validate_inventory_page(invalid, "options_graphics", 0)
            except ValueError:
                pass
            else:
                raise AssertionError("Incomplete or malformed stock menu page accepted")
        for invalid in ([], [item, dict(item)], [dict(item, enabled=False)]):
            try:
                one_control(invalid, text="@MENU_APPLY")
            except ValueError:
                pass
            else:
                raise AssertionError("Missing/ambiguous/disabled stock control accepted")
        assert_persisted_scale({"fov_scale": 1.25, "weapon": 27}, {"settings": {"cg_fovScale": "1.25"}}, 1.25)
        for state, archive in (({"fov_scale": 1.0}, {"settings": {"cg_fovScale": "1.25"}}),
                               ({"fov_scale": 1.25}, {"settings": {"cg_fovScale": "1"}}),
                               ({"fov_scale": float("nan")}, {"settings": {"cg_fovScale": "1.25"}})):
            try:
                assert_persisted_scale(state, archive, 1.25)
            except ValueError:
                pass
            else:
                raise AssertionError("Native map preference reset accepted before weapon recalibration")
        attempted, independent = [], {}

        def category_policy(name, proof):
            attempted.append(name)
            if name in ("options_graphics", "options_sound"):
                raise ValueError("Fixture fault: Apply remains unverified")
            proof["passed"] = True

        assert not collect_independent_categories(OPTIONS_CATEGORIES, independent, category_policy)
        assert attempted == list(OPTIONS_CATEGORIES) and len(independent) == 12
        assert not independent["options_graphics"]["passed"] and "error" in independent["options_graphics"]
        assert independent["controls_multi"]["passed"]

        class ActionReplyFixture:
            def __init__(self, result):
                self.result, self.report = result, {}

            def call(self, command):
                return {"result": "stock item details", "action": "native compiled metadata"} if " details " in command else {"result": self.result}

        assert stock_action(ActionReplyFixture("stock key dispatched"), "options_graphics", item, K_RIGHT)["reply"]["result"] == "stock key dispatched"
        assert stock_action(ActionReplyFixture("stock action executed"), "options_graphics", item)["reply"]["result"] == "stock action executed"
        old_endpoint = ActionReplyFixture("stock action executed")
        try:
            stock_action(old_endpoint, "options_graphics", item, K_RIGHT)
        except ValueError:
            assert len(old_endpoint.report["stock_action_attempts"]) == 1
        else:
            raise AssertionError("Old direct-widget shortcut accepted as native key dispatch")
        confirm_item = dict(item, name="confirm", text="@MENU_YES")
        native_confirm = {"item": confirm_item, "details": {"action": '"exec" "vid_restart" ; "close" "self" ;'}}
        native_cancel = {"item": dict(item, index=1, name="cancel", text="@MENU_NO"),
                         "details": {"action": '"close" "vid_restart_popmenu" ;'}}
        assert select_restart_confirmation([native_confirm, native_cancel], "vid_restart") is native_confirm
        for controls in ([native_cancel], [native_confirm, dict(native_confirm)],
                         [{"item": dict(confirm_item, enabled=False), "details": native_confirm["details"]}],
                         [{"item": confirm_item, "details": {"action": '"exec" "snd_restart" ;'}}]):
            try:
                select_restart_confirmation(controls, "vid_restart")
            except ValueError:
                pass
            else:
                raise AssertionError("Missing/ambiguous/disabled/wrong-command restart confirmation accepted")
        print("FOV geometry/clamp plus bounded native archive/menu parsing policies passed; these selftests do not prove native UI actions")
        return 0
    project = Path(__file__).resolve().parent.parent
    run_id = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    output = (args.output or project / "results" / ("native-fov-" + run_id)).resolve()
    if not 1024 <= args.client_port <= 65535 or output.exists() or not output.is_relative_to(project / "results") or output == project / "results":
        parser.error("Use a valid port and a fresh project results directory")
    output.mkdir(parents=True)
    fixture = ManagedHostFixture(project, output, args.client_port)
    fixture.report["fov_probe_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    fixture.report["extended_requested"] = args.extended
    preferences = None
    try:
        preferences = fixture.report["interactive_preferences_before"] = interactive_preferences(project)
        run(fixture, args.extended)
    except Exception as error:
        fixture.report["errors"].append(str(error))
    finally:
        fixture.report["test_elapsed_seconds"] = time.monotonic() - fixture.started
        fixture.close()
        try:
            fixture.report["interactive_preferences_after"] = interactive_preferences(project)
            fixture.report["interactive_preferences_unchanged"] = preferences is not None and preferences == fixture.report["interactive_preferences_after"]
            if not fixture.report["interactive_preferences_unchanged"]:
                fixture.report["errors"].append("The isolated benchmark FOV/Options fixture changed interactive preferences")
                fixture.report["passed"] = False
        except (OSError, ValueError) as error:
            fixture.report["errors"].append("Interactive preference verification: " + str(error))
            fixture.report["passed"] = False
        (output / "result.json").write_text(json.dumps(fixture.report, indent=2), encoding="utf8")
    print(json.dumps({"passed": fixture.report["passed"], "errors": fixture.report["errors"], "output": str(output / "result.json")}, indent=2))
    return 0 if fixture.report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
