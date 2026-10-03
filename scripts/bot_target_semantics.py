"""Evaluate actual BW target-loop source and the separately staged math hoist.

This is a small strict GSC-subset interpreter, not a geometry simulator or a
native compiler. Trace answers are controlled inputs; actual script statements
drive target updates, reaction/remembered state, random calls, and selection.
The staged patch is applied only in memory. Its checks parse the real upstream
getConeDot helper and compare it with the staged normalize/dot expression.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import itertools
import json
import math
from pathlib import Path
import random
import re
import time


FUNCTIONS = ("createTargetObj", "updateAimOffset", "targetObjUpdateTraced",
             "targetObjUpdateNoTrace", "target_loop")
BONES = ("j_head", "j_ankle_le", "j_ankle_ri")
PURE_CONE_CALLS = ("cone", "normalize", "forward", "dot")
LEXER = re.compile(r'\s+|//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'
                   r'\d+(?:\.\d+)?|[A-Za-z_]\w*|\+\+|--|==|!=|>=|<=|&&|\|\||'
                   r'[+*/%\-]=|[{}()[\].,;=+*/%<>!\\:\-]', re.S)


def extract(source, name):
    start = re.search(r"(?m)^" + re.escape(name) + r"\s*\([^)]*\)\s*\{", source)
    if not start:
        raise ValueError(f"Missing actual GSC function {name}")
    pos, depth = start.start(), 0
    while pos < len(source):
        match = LEXER.match(source, pos)
        if not match:
            raise ValueError(f"Unsupported GSC token in {name}: {source[pos:pos + 40]!r}")
        token = match.group()
        if token == "{":
            depth += 1
        elif token == "}":
            depth -= 1
            if depth == 0:
                return source[start.start():match.end()]
        pos = match.end()
    raise ValueError(f"Unterminated GSC function {name}")


def tokens(source):
    result, pos = [], 0
    while pos < len(source):
        match = LEXER.match(source, pos)
        if not match:
            raise ValueError(f"Unsupported GSC syntax: {source[pos:pos + 40]!r}")
        token = match.group()
        if not token.isspace() and not token.startswith(("//", "/*")):
            result.append(token)
        pos = match.end()
    return result


class Parser:
    precedence = {"||": 1, "&&": 2, "==": 3, "!=": 3, ">": 4, "<": 4,
                  ">=": 4, "<=": 4, "+": 5, "-": 5, "*": 6, "/": 6, "%": 6}

    def __init__(self, source):
        self.values, self.pos = tokens(source), 0

    def peek(self):
        return self.values[self.pos] if self.pos < len(self.values) else ""

    def pop(self, expected=None):
        value = self.peek()
        if not value or (expected is not None and value != expected):
            raise ValueError(f"Expected {expected!r}, got {value!r} near {self.values[self.pos:self.pos + 10]}")
        self.pos += 1
        return value

    def arguments(self):
        self.pop("(")
        values = []
        while self.peek() != ")":
            values.append(self.expression())
            if self.peek() != ",":
                break
            self.pop(",")
        self.pop(")")
        return values

    def expression(self, minimum=0):
        value = self.pop()
        if value in ("!", "-"):
            left = ("unary", value, self.expression(7))
        elif value == "(":
            left = self.expression()
            if self.peek() == ",":
                self.pop(","); y = self.expression(); self.pop(","); z = self.expression()
                left = ("vector", left, y, z)
            self.pop(")")
        elif value == "[":
            self.pop("]"); left = ("array",)
        elif value.startswith('"'):
            left = ("literal", json.loads(value))
        elif value[0].isdigit():
            left = ("literal", float(value) if "." in value else int(value))
        elif value in ("undefined", "true", "false"):
            left = ("literal", {"undefined": None, "true": True, "false": False}[value])
        elif self.peek() == "(":
            left = ("call", value, None, self.arguments())
        elif re.fullmatch(r"[A-Za-z_]\w*", value):
            left = ("variable", value)
        else:
            raise ValueError(f"Unsupported expression prefix {value!r}")
        while True:
            value = self.peek()
            if value == ".":
                self.pop(); left = ("field", left, self.pop())
            elif value == "[":
                self.pop(); key = self.expression(); self.pop("]"); left = ("index", left, key)
            elif re.fullmatch(r"[A-Za-z_]\w*", value) and self.pos + 1 < len(self.values) and self.values[self.pos + 1] == "(":
                self.pop(); left = ("call", value, left, self.arguments())
            elif value in self.precedence and self.precedence[value] >= minimum:
                self.pop(); left = ("binary", value, left, self.expression(self.precedence[value] + 1))
            else:
                return left

    def simple(self):
        value = self.expression()
        if self.peek() in ("=", "+=", "-=", "*=", "/="):
            operation = self.pop()
            return ("assign", operation, value, self.expression())
        if self.peek() in ("++", "--"):
            return ("increment", self.pop(), value)
        return ("evaluate", value)

    def statement(self):
        if self.peek() == "{":
            self.pop(); block = []
            while self.peek() != "}":
                block.append(self.statement())
            self.pop("}"); return ("block", block)
        if self.peek() == "if":
            self.pop(); self.pop("("); condition = self.expression(); self.pop(")")
            yes, no = self.statement(), None
            if self.peek() == "else":
                self.pop(); no = self.statement()
            return ("if", condition, yes, no)
        if self.peek() == "for":
            self.pop(); self.pop("("); initial = self.simple(); self.pop(";")
            condition = self.expression(); self.pop(";"); advance = self.simple(); self.pop(")")
            return ("for", initial, condition, advance, self.statement())
        if self.peek() in ("return", "continue"):
            operation = self.pop()
            value = self.expression() if self.peek() != ";" else None
            self.pop(";"); return (operation, value)
        value = self.simple(); self.pop(";"); return value

    def function(self):
        name = self.pop(); self.pop("("); params = []
        while self.peek() != ")":
            params.append(self.pop())
            if self.peek() != ",":
                break
            self.pop(",")
        self.pop(")"); body = self.statement()
        if self.peek():
            raise ValueError(f"Trailing unsupported syntax after {name}")
        return params, body


@dataclass(frozen=True)
class Vector:
    x: float
    y: float
    z: float
    entity: int | None = None
    bone: str | None = None

    def __add__(self, other):
        return Vector(self.x + other.x, self.y + other.y, self.z + other.z, self.entity, self.bone)

    def __sub__(self, other):
        return Vector(self.x - other.x, self.y - other.y, self.z - other.z, self.entity, self.bone)

    def __mul__(self, scalar):
        return Vector(self.x * scalar, self.y * scalar, self.z * scalar)

    def coordinates(self):
        return (self.x, self.y, self.z)


def normalize_vector(value):
    length = math.sqrt(value.x * value.x + value.y * value.y + value.z * value.z)
    inverse = 1 / length if length > 0 else 1
    return Vector(value.x * inverse, value.y * inverse, value.z * inverse, value.entity, value.bone)


def forward_vector(angles):
    pitch, yaw = math.radians(angles.x), math.radians(angles.y)
    return Vector(math.cos(pitch) * math.cos(yaw),
                  math.cos(pitch) * math.sin(yaw), -math.sin(pitch))


def vector_dot(left, right):
    return left.x * right.x + left.y * right.y + left.z * right.z


@dataclass(eq=False)
class Entity:
    number: int
    fields: dict


class Returned(Exception):
    def __init__(self, value):
        self.value = value


class Continued(Exception):
    pass


def field(value, name):
    if name == "size" and isinstance(value, (dict, list)):
        return len(value)
    return (value.fields if isinstance(value, Entity) else value).get(name)


def binary(operation, left, right):
    if operation == "+" and (isinstance(left, str) or isinstance(right, str)):
        return str(left) + str(right)
    if operation == "+": return left + right
    if operation == "-": return left - right
    if operation == "*": return left * right
    if operation == "/": return left / right
    if operation == "%": return left % right
    if operation == "==": return left == right
    if operation == "!=": return left != right
    if operation == ">": return left > right
    if operation == "<": return left < right
    if operation == ">=": return left >= right
    if operation == "<=": return left <= right
    raise ValueError(f"Unsupported operator {operation}")


class Runtime:
    def __init__(self, functions, rows, existing=None, current=None, override=None, skill=None, script=None,
                 geometry=False, observer_origin=(0, 0, 0), observer_angles=(0, 0, 0)):
        self.functions, self.rows, self.calls = functions, {row["id"]: row for row in rows}, []
        self.geometry, self.origin_reads = geometry, 0
        self.observer_angles = Vector(*observer_angles)
        self.random = random.Random(93)
        self.now = 1000
        defaults = dict(fov=.5, remember_time=100, init_react_time=100, ads_fov_multi=.5,
                        dist_start=100, dist_max=200, aim_offset_time=1, aim_offset_amount=2)
        defaults.update(skill or {})
        self.entities = {row["id"]: Entity(row["id"], dict(
            origin=Vector(*row.get("origin", (row.get("distance", 64), 0, 0)), entity=row["id"]),
            team=row.get("team", "axis"), sessionstate=row.get("session", "playing"))) for row in rows}
        targets = {}
        self.me = Entity(0, {"origin": Vector(*observer_origin, entity=0), "team": "allies",
                             "pers": {"bots": {"skill": defaults}}, "bot": {
                                 "targets": targets, "target": None, "target_this_frame": None,
                                 "script_target": None, "script_target_offset": None, "cur_weap_dist_multi": 1}})
        for number, overrides in (existing or {}).items():
            targets[str(number)] = dict(entity=self.entities[number], last_seen_pos=Vector(8, 9, 10),
                dist=number * 32, time=500, trace_time=50, no_trace_time=0,
                trace_time_time=900, rand=7, didlook=True, offset=None, bone=None,
                aim_offset=Vector(1, 1, 1), aim_offset_base=None, **{})
            targets[str(number)].update(overrides)
        bot = self.me.fields["bot"]
        bot["target"] = targets.get(str(current)) if current != "orphan" else {}
        bot["target_this_frame"] = self.entities.get(override)
        if script is not None:
            bot["script_target"] = self.entities[script]
            bot["script_target_offset"] = Vector(1, 2, 3)
        self.level = {"players": list(self.entities.values()), "teambased": True,
                      "smokeradius": 1, "bots_maxknifedistance": 32}

    def evaluate(self, node, local):
        operation = node[0]
        if operation == "literal": return node[1]
        if operation == "variable": return local.get(node[1])
        if operation == "array": return {}
        if operation == "vector": return Vector(*(self.evaluate(v, local) for v in node[1:]))
        if operation == "field":
            receiver = self.evaluate(node[1], local)
            if receiver is self.me and node[2] == "origin":
                self.origin_reads += 1
            return field(receiver, node[2])
        if operation == "index":
            container, key = self.evaluate(node[1], local), self.evaluate(node[2], local)
            return container[key] if isinstance(container, list) else container.get(key)
        if operation == "unary":
            value = self.evaluate(node[2], local)
            return not value if node[1] == "!" else -value
        if operation == "binary":
            left = self.evaluate(node[2], local)
            if node[1] == "&&": return bool(left) and bool(self.evaluate(node[3], local))
            if node[1] == "||": return bool(left) or bool(self.evaluate(node[3], local))
            return binary(node[1], left, self.evaluate(node[3], local))
        if operation == "call":
            receiver = self.evaluate(node[2], local) if node[2] is not None else local["self"]
            args = [self.evaluate(arg, local) for arg in node[3]]
            return self.call(node[1], receiver, args)
        raise ValueError(f"Unsupported expression {operation}")

    def assign(self, node, value, local):
        if node[0] == "variable": local[node[1]] = value; return
        container = self.evaluate(node[1], local)
        if node[0] == "field":
            container = container.fields if isinstance(container, Entity) else container
            container[node[2]] = value; return
        if node[0] == "index":
            key = self.evaluate(node[2], local)
            if value is None: container.pop(key, None)
            else: container[key] = value
            return
        raise ValueError(f"Unsupported assignment {node[0]}")

    def statement(self, node, local):
        operation = node[0]
        if operation == "block":
            for child in node[1]: self.statement(child, local)
        elif operation == "if":
            branch = node[2] if self.evaluate(node[1], local) else node[3]
            if branch is not None: self.statement(branch, local)
        elif operation == "for":
            self.statement(node[1], local)
            loops = 0
            while self.evaluate(node[2], local):
                loops += 1
                if loops > 128: raise ValueError("Unbounded fixture loop")
                try: self.statement(node[4], local)
                except Continued: pass
                self.statement(node[3], local)
        elif operation == "assign":
            value = self.evaluate(node[3], local)
            if node[1] != "=": value = binary(node[1][0], self.evaluate(node[2], local), value)
            self.assign(node[2], value, local)
        elif operation == "increment":
            self.assign(node[2], self.evaluate(node[2], local) + (1 if node[1] == "++" else -1), local)
        elif operation == "evaluate": self.evaluate(node[1], local)
        elif operation == "continue": raise Continued()
        elif operation == "return": raise Returned(self.evaluate(node[1], local) if node[1] else None)
        else: raise ValueError(f"Unsupported statement {operation}")

    def call(self, name, receiver, args):
        if name in self.functions:
            if name == "getConeDot":
                self.calls.append(("cone", args[0].entity))
            params, code = self.functions[name]
            local = dict(zip(params, args)); local.update(self=receiver, level=self.level)
            try: self.statement(code, local)
            except Returned as result: return result.value
            return None
        if name == "isdefined": return args[0] is not None
        if name == "int": return int(args[0])
        if name == "spawnstruct": return {}
        if name == "getarraykeys": return list(args[0])
        if name == "gettime": return self.now
        if name == "getEyePos": return receiver.fields["origin"]
        if name == "getplayerangles": return self.observer_angles
        if name == "playerads": return self.rows.get(0, {}).get("ads", 0)
        if name == "getentitynumber": return receiver.number
        if name == "IsPlayerModelOK": return self.rows[receiver.number].get("model", True)
        if name == "isalive": return self.rows[args[0].number].get("alive", True)
        if name == "distancesquared":
            a, b = args
            value = (a.x - b.x) ** 2 + (a.y - b.y) ** 2 + (a.z - b.z) ** 2
            self.calls.append(("distance", b.entity, a.coordinates(), b.coordinates(), value))
            return value
        if name == "gettagorigin":
            self.calls.append(("tag", receiver.number, args[0]))
            return Vector(0, 0, 0, receiver.number, args[0])
        if name in ("sighttracepassed", "bullettrace"):
            point = args[1]; row = self.rows[point.entity]; bone = BONES.index(point.bone)
            self.calls.append(("sight" if name == "sighttracepassed" else "bullet", point.entity, point.bone))
            if name == "sighttracepassed": return row["sight"][bone]
            result = row["bullet"][bone]
            return {"fraction": 1 if result == "pass" else .5, "surfacetype": "glass" if result == "glass" else "rock"}
        if name == "bullettracepassed":
            self.calls.append(("script_bullet", args[1].entity))
            return self.rows[args[1].entity].get("script_pass", True)
        if name == "SmokeTrace":
            self.calls.append(("smoke", args[1].entity))
            return self.rows[args[1].entity].get("smoke", True)
        if name == "getConeDot":
            self.calls.append(("cone", args[0].entity))
            return self.rows[args[0].entity].get("dot", 1)
        if name == "vectornormalize":
            value = normalize_vector(args[0])
            self.calls.append(("normalize", value.entity, args[0].coordinates(), value.coordinates()))
            return value
        if name == "anglestoforward":
            value = forward_vector(args[0])
            self.calls.append(("forward", args[0].coordinates(), value.coordinates()))
            return value
        if name == "vectordot":
            direction, forward = args
            value = vector_dot(direction, forward) if self.geometry else self.rows[direction.entity].get("dot", 1)
            self.calls.append(("dot", direction.entity, direction.coordinates(), forward.coordinates(), value))
            return value
        if name in ("randomint", "randomfloatrange"):
            value = self.random.randrange(args[0]) if name == "randomint" else self.random.uniform(*args)
            self.calls.append((name, *args, value)); return value
        if name == "notify": self.calls.append(("notify", *args)); return None
        raise ValueError(f"Unsupported native/function call {name}")

    def run(self):
        self.call("target_loop", self.me, [])

    def outcome(self):
        def normalize(value):
            if isinstance(value, Entity): return {"entity_number": value.number}
            if isinstance(value, Vector): return [value.x, value.y, value.z]
            if isinstance(value, dict): return {key: normalize(v) for key, v in value.items()}
            return value
        return normalize(self.me.fields["bot"]), [call for call in self.calls if call[0] in ("randomint", "randomfloatrange", "notify")]


def compile_functions(source, utility=None):
    result = {name: Parser(extract(source, name)).function() for name in FUNCTIONS}
    if utility is not None:
        result["getConeDot"] = Parser(extract(utility, "getConeDot")).function()
    return result


def verify_call_contract(runtime):
    for number, row in runtime.rows.items():
        calls = [call for call in runtime.calls if len(call) > 1 and call[1] == number]
        sights = [call[2] for call in calls if call[0] == "sight"]
        bullets = [call[2] for call in calls if call[0] == "bullet"]
        for actual, answers in ((sights, row["sight"]), (bullets, [x != "blocked" for x in row["bullet"]])):
            assert actual == list(BONES[:len(actual)]), ("trace bone order", number, actual)
            assert len(actual) <= 3
            if actual:
                assert not any(answers[:len(actual) - 1]), ("trace after OR already passed", number, actual)
                assert len(actual) == 3 or answers[len(actual) - 1], ("OR stopped before success", number, actual)
        assert not bullets or any(row["sight"]), ("bullet trace without sight", number)
        tags = [call[2] for call in calls if call[0] == "tag"]
        assert len(tags) == len(set(tags)) <= 3, ("duplicate bone fetch", number, tags)
        assert set(tags) == set(sights + bullets), ("unused bone fetch", number, tags)
        assert not bullets or calls.index(("sight", number, sights[-1])) < calls.index(("bullet", number, bullets[0]))


def apply_staged_patch(source, patch, reverse=False):
    """Strict, single-file unified patch; no filesystem mutation or fuzz."""
    lines = patch.splitlines()
    expected = "config/bots/maps/mp/bots/_bot_internal.gsc"
    if lines[:2] != ["--- a/" + expected, "+++ b/" + expected]:
        raise ValueError("Staged patch must target exactly the current bot overlay")
    source_lines, result, cursor, pos, hunks = source.splitlines(), [], 0, 2, 0
    while pos < len(lines):
        match = re.fullmatch(r"@@ -(\d+),(\d+) \+(\d+),(\d+) @@", lines[pos])
        if not match:
            raise ValueError(f"Unsupported staged patch hunk: {lines[pos]!r}")
        old_start, old_count, new_start, new_count = map(int, match.groups())
        if reverse:
            old_start, old_count, new_start, new_count = new_start, new_count, old_start, old_count
        start = old_start - 1
        if start < cursor or start > len(source_lines):
            raise ValueError("Staged patch hunk order/range changed")
        result.extend(source_lines[cursor:start])
        if new_start != len(result) + 1:
            raise ValueError("Staged patch new hunk offset changed")
        pos += 1
        removed, inserted = [], []
        while pos < len(lines) and not lines[pos].startswith("@@"):
            line = lines[pos]
            if not line or line[0] not in " +-":
                raise ValueError(f"Unsupported staged patch line {line!r}")
            if reverse and line[0] in "+-":
                line = ("-" if line[0] == "+" else "+") + line[1:]
            if line[0] in " -":
                removed.append(line[1:])
            if line[0] in " +":
                inserted.append(line[1:])
            pos += 1
        if len(removed) != old_count or len(inserted) != new_count:
            raise ValueError("Staged patch declared hunk extent changed")
        if source_lines[start:start + old_count] != removed:
            raise ValueError(f"Staged patch baseline mismatch at overlay line {old_start}")
        result.extend(inserted)
        cursor, hunks = start + old_count, hunks + 1
    if hunks != 4:
        raise ValueError("Staged math hoist must contain exactly four reviewed hunks")
    result.extend(source_lines[cursor:])
    return "\n".join(result) + ("\n" if source.endswith("\n") else "")


def verify_staged_source(overlay, staged, utility):
    """Restrict the candidate to the reviewed field/math substitutions."""
    expected = extract(overlay, "target_loop")
    replacements = (
        ("myAngles = self getplayerangles();",
         "myAngles = self getplayerangles(); myOrigin = self.origin; myForward = anglestoforward( myAngles );"),
        ("distancesquared( self.origin, ent.origin )", "distancesquared( myOrigin, ent.origin )"),
        ("distancesquared( self.origin, player.origin )", "distancesquared( myOrigin, player.origin )"),
        ("getConeDot( player.origin, self.origin, myAngles )",
         "vectordot( vectornormalize( player.origin - myOrigin ), myForward )"),
    )
    for old, new in replacements:
        if expected.count(old) != 1:
            raise ValueError(f"Reviewed baseline substitution is not unique: {old}")
        expected = expected.replace(old, new)
    assert tokens(extract(staged, "target_loop")) == tokens(expected), "Staged target behavior changed beyond the math hoist"
    old_loop, new_loop = extract(overlay, "target_loop"), extract(staged, "target_loop")
    assert staged == overlay.replace(old_loop, new_loop, 1), "Staged patch changed outside target_loop"
    assert tokens(extract(utility, "getConeDot")) == tokens("""
        getConeDot( to, from, dir ) {
            dirToTarget = vectornormalize( to - from );
            forward = anglestoforward( dir );
            return vectordot( dirToTarget, forward );
        }"""), "Actual upstream cone math changed"
    # These synchronous helpers do not yield or write the observer's origin.
    # The native trace/model/angle getters remain controlled read-only inputs.
    blocks = [extract(staged, name) for name in FUNCTIONS]
    blocks += [extract(utility, name) for name in
               ("getEyePos", "getEyeHeight", "IsPlayerModelOK", "SmokeTrace", "RaySphereIntersect", "getConeDot")]
    for block in blocks:
        values = tokens(block)
        assert not set(values) & {"wait", "waittill", "waittillmatch", "waittillframeend", "thread"}, "Decision now yields"
        assert not re.search(r"\bself\s*\.\s*origin\s*(?:=(?!=)|[+*/-]=|\+\+|--)", block), "Decision writes observer origin"
    assert tokens(extract(staged, "target")) == tokens(extract(overlay, "target")), "50 ms decision cadence changed"


def verify_staged_runtime(before, after, label):
    assert before.outcome() == after.outcome(), (label, "staged state/RNG/events", before.outcome(), after.outcome())
    observable = lambda runtime: [call for call in runtime.calls if call[0] not in PURE_CONE_CALLS]
    assert observable(before) == observable(after), (label, "candidate/distance/LOS/RNG order changed",
                                                    observable(before), observable(after))
    candidates = lambda runtime: [call for call in runtime.calls if call[0] in ("normalize", "dot")]
    assert candidates(before) == candidates(after), (label, "normalize/dot operand or result changed")
    before_forward = [call for call in before.calls if call[0] == "forward"]
    after_forward = [call for call in after.calls if call[0] == "forward"]
    assert len(after_forward) == 1, (label, "forward is not cached exactly once")
    assert all(call == after_forward[0] for call in before_forward), (label, "cached forward differs")
    assert len(before_forward) == sum(call[0] == "normalize" for call in before.calls), (label, "baseline cone helper not executed")
    assert after.origin_reads == 1, (label, "observer origin not captured exactly once", after.origin_reads)
    verify_call_contract(after)
    return len(before_forward) - len(after_forward), before.origin_reads - after.origin_reads


def regression(project, staged_patch=None):
    old_path = project / "upstream/bot_warfare/maps/mp/bots/_bot_internal.gsc"
    new_path = project / "config/bots/maps/mp/bots/_bot_internal.gsc"
    original, optimized = old_path.read_text(), new_path.read_text()
    utility_path = project / "upstream/bot_warfare/maps/mp/bots/_bot_utility.gsc"
    utility = utility_path.read_text()
    baseline = optimized
    if staged_patch is not None:
        patch = staged_patch.read_text()
        if "myForward" in tokens(extract(optimized, "target_loop")):
            baseline = apply_staged_patch(optimized, patch, reverse=True)
            staged = apply_staged_patch(baseline, patch)
            assert staged == optimized, "Applied overlay differs from the reviewed staged patch"
        else:
            staged = apply_staged_patch(baseline, patch)
    else:
        staged = None
    if staged is not None:
        verify_staged_source(baseline, staged, utility)
    for name in FUNCTIONS[:-1] + ("target",):
        assert tokens(extract(original, name)) == tokens(extract(optimized, name)), f"Untouched BW behavior changed: {name}"
    old_code, new_code = compile_functions(original), compile_functions(optimized)
    # Baseline executes the actual parsed getConeDot; the candidate executes
    # the actual staged expression. Controlled cone answers still exhaust the
    # earlier branch truth tables; separate cases below use modeled vectors.
    baseline_math = compile_functions(baseline, utility) if staged is not None else None
    staged_math = compile_functions(staged, utility) if staged is not None else None
    count, reductions = 0, {name: 0 for name in ("tag", "bullet", "sight")}
    staged_count, staged_geometry_count, forward_saved, origin_reads_saved = 0, 0, 0, 0

    def check(label, rows, **kwargs):
        nonlocal count, staged_count, forward_saved, origin_reads_saved
        before, after = Runtime(old_code, rows, **kwargs), Runtime(new_code, rows, **kwargs)
        before.run(); after.run()
        assert before.outcome() == after.outcome(), (label, before.outcome(), after.outcome())
        verify_call_contract(after)
        for name in reductions:
            old = sum(call[0] == name for call in before.calls)
            new = sum(call[0] == name for call in after.calls)
            assert new <= old, ("more native calls", label, name, old, new)
            reductions[name] += old - new
        if staged is not None:
            baseline, candidate = Runtime(baseline_math, rows, **kwargs), Runtime(staged_math, rows, **kwargs)
            baseline.run(); candidate.run()
            forward, reads = verify_staged_runtime(baseline, candidate, label)
            forward_saved += forward; origin_reads_saved += reads; staged_count += 1
        count += 1
        return before, after

    # Exhaust every sight/bullet truth assignment, including independent bones
    # and glass, across cheap-gate, smoke/close, memory and frame-override paths.
    for sights, bullets, inside, smoke, close, memory, forced in itertools.product(
            itertools.product((False, True), repeat=3), itertools.product(("blocked", "pass", "glass"), repeat=3),
            (False, True), (False, True), (False, True), ("new", "seen", "lost"), (False, True)):
        row = dict(id=1, sight=sights, bullet=bullets, dot=1 if inside else -1,
                   smoke=smoke, distance=4 if close else 64)
        old = None if memory == "new" else {1: {"trace_time": 50 if memory == "seen" else 0, "no_trace_time": 50}}
        check("visibility_truth_table", [row], existing=old, override=1 if forced else None)

    # Remembered boundaries, reaction equality, invalid candidates, ADS FOV,
    # range attenuation, an existing main target and multi-player tie selection.
    for no_trace, age, eligible, ads, distance, forced in itertools.product((50, 51, 100), (99, 100, 101),
            ("playing", "dead", "ally", "bad_model", "spectator"), (0, 1), (4, 100, 150, 201), (False, True)):
        row = dict(id=1, sight=(False, True, False), bullet=("pass", "blocked", "blocked"),
                   dot=.3, smoke=True, distance=distance, alive=eligible != "dead",
                   team="allies" if eligible == "ally" else "axis", model=eligible != "bad_model",
                   session="spectator" if eligible == "spectator" else "playing")
        observer = dict(id=0, sight=(False,) * 3, bullet=("blocked",) * 3, team="allies", ads=ads)
        before, after = check("state_boundaries", [observer, row], existing={1: {
            "trace_time": 0, "no_trace_time": no_trace, "time": 1000 - age}}, current=1, override=1 if forced else None)
        if forced and eligible != "playing":
            assert after.me.fields["bot"]["target_this_frame"] is after.entities[1], "Invalid candidate consumed override"
    for difficulty, age_delta, offset_defined in itertools.product((1500, 750, 500, 100), (-1, 0, 1), (False, True)):
        check("four_difficulty_reaction_boundary", [dict(id=1, sight=(True, False, False),
              bullet=("glass", "blocked", "blocked"), dot=1)],
              existing={1: {"time": 1000 - difficulty - age_delta,
                  "aim_offset_base": Vector(1, 2, 3) if offset_defined else None}},
              skill={"init_react_time": difficulty}, current=1)
    for main, reverse, remembered in itertools.product((None, 1, 2, "orphan"), (False, True), (False, True)):
        rows = [dict(id=i, sight=(i == 2, i == 1, False), bullet=("glass", "blocked", "blocked"),
                     distance=64 if i == 1 else 32, dot=-1 if remembered else 1) for i in (1, 2)]
        check("retained_and_best_targets", rows[::-1] if reverse else rows,
              existing={1: {}, 2: {}}, current=main)
    for passed, smoked, remembered in itertools.product((False, True), repeat=3):
        check("script_target", [dict(id=1, sight=(False,) * 3, bullet=("blocked",) * 3,
                                    script_pass=passed, smoke=smoked, dot=-1)], script=1,
              existing={1: {}} if remembered else None, current=1 if remembered else None)

    # Multiple complete 50ms decisions prove aging/remembrance over time and
    # exactly preserved random and new_enemy event streams, not just a bool.
    rows = [dict(id=i, sight=(True, False, False), bullet=("pass", "blocked", "blocked")) for i in (1, 2)]
    before, after = Runtime(old_code, rows, existing={1: {}, 2: {}}, current=1), Runtime(new_code, rows, existing={1: {}, 2: {}}, current=1)
    for phase in range(12):
        for runtime in (before, after):
            runtime.now = 1000 + phase * 50
            for number, row in runtime.rows.items():
                row["dot"] = 1 if phase < 3 or phase > 8 else -1
                row["sight"] = (phase < 3 or phase > 8, False, False)
                if phase in (5, 10): runtime.me.fields["bot"]["target_this_frame"] = runtime.entities[number]
            runtime.run()
        assert before.outcome() == after.outcome(), ("multi_tick_state", phase, before.outcome(), after.outcome())
        count += 1

    staged_report = None
    if staged is not None:
        def geometry_check(label, rows, **kwargs):
            nonlocal staged_count, staged_geometry_count, forward_saved, origin_reads_saved
            before = Runtime(baseline_math, rows, geometry=True, **kwargs)
            after = Runtime(staged_math, rows, geometry=True, **kwargs)
            before.run(); after.run()
            forward, reads = verify_staged_runtime(before, after, label)
            forward_saved += forward; origin_reads_saved += reads
            staged_count += 1; staged_geometry_count += 1
            return before, after

        offsets = ((0, 0, 0), (64, 0, 0), (-64, 0, 0), (0, 64, 0),
                   (32, 32, 48), (-17, 81, -64), (.001, -.002, .003))
        origins = ((0, 0, 0), (17, -29, 63), (10000, -10000, 8192))
        angles = ((0, 0, 0), (0, 90, 0), (30, 45, 0), (-65, -135, 90), (0, 359, 0))
        for origin, angle, offset, boundary, forced, remembered in itertools.product(
                origins, angles, offsets, (-1, 0, 1), (False, True), (False, True)):
            position = tuple(a + b for a, b in zip(origin, offset))
            actual_dot = vector_dot(normalize_vector(Vector(*offset)), forward_vector(Vector(*angle)))
            fov = (actual_dot if boundary == 0 else math.nextafter(
                actual_dot, -math.inf if boundary < 0 else math.inf))
            rows = [dict(id=1, origin=position, sight=(False, True, False),
                         bullet=("glass", "blocked", "blocked"), smoke=True),
                    dict(id=2, origin=(origin[0] + 64, origin[1] - 32, origin[2]),
                         sight=(True, False, False), bullet=("blocked", "pass", "blocked"))]
            geometry_check("modeled_pitch_yaw_origin_zero_and_fov_boundary", rows,
                           observer_origin=origin, observer_angles=angle, skill={"fov": fov},
                           existing={1: {"trace_time": 50}} if remembered else None,
                           override=1 if forced else None)

        for population, eligibility in itertools.product((0, 1, 4, 15), ("playing", "ally", "dead", "bad_model")):
            rows = [dict(id=i, origin=(32 + i, i * 3, i - 8),
                         sight=(False, False, True), bullet=("blocked", "glass", "blocked"),
                         team="allies" if eligibility == "ally" else "axis",
                         alive=eligibility != "dead", model=eligibility != "bad_model")
                    for i in range(1, population + 1)]
            before, after = geometry_check("population_and_invalid_candidates", rows)
            if population == 15 and eligibility == "playing":
                assert sum(call[0] == "forward" for call in before.calls) == 15
                assert sum(call[0] == "forward" for call in after.calls) == 1
                assert before.origin_reads == 30 and after.origin_reads == 1
        for passed, smoked, current in itertools.product((False, True), (False, True), (None, 1)):
            geometry_check("script_target_only", [dict(id=1, origin=(31, -43, 65),
                team="allies", script_pass=passed, smoke=smoked,
                sight=(False,) * 3, bullet=("blocked",) * 3)],
                script=1, existing={1: {}} if current else None, current=current,
                observer_origin=(13, 3, -2), observer_angles=(17, 91, 0))

        # Complete successive decisions must recapture pose each time. Retain
        # target memory, RNG, forced-target consumption and the 50 ms clock.
        rows = [dict(id=i, origin=(64, (i - 1) * 10, i * 2),
                     sight=(True, False, False), bullet=("pass", "blocked", "blocked"))
                for i in (1, 2)]
        before = Runtime(baseline_math, rows, geometry=True, existing={1: {}, 2: {}}, current=1)
        after = Runtime(staged_math, rows, geometry=True, existing={1: {}, 2: {}}, current=1)
        for phase in range(20):
            for runtime in (before, after):
                runtime.now = 1000 + phase * 50
                runtime.calls, runtime.origin_reads = [], 0
                runtime.observer_angles = Vector(phase * 3 - 30, phase * 27, 0)
                runtime.me.fields["origin"] = Vector(phase * 4 - 20, phase - 3, phase * 2, entity=0)
                for number, row in runtime.rows.items():
                    row["sight"] = (phase % 4 != 2, False, False)
                    if phase in (3, 11):
                        runtime.me.fields["bot"]["target_this_frame"] = runtime.entities[number]
                runtime.run()
            forward, reads = verify_staged_runtime(before, after, ("successive_50ms_decisions", phase))
            forward_saved += forward; origin_reads_saved += reads
            staged_count += 1; staged_geometry_count += 1

        # Negative controls establish that equality really depends on actual
        # cone operands/order and independent LOS ORs, rather than shared stubs.
        mutations = {
            "wrong_vector_direction": staged.replace("player.origin - myOrigin", "myOrigin - player.origin", 1),
            "unnormalized_dot": staged.replace("vectornormalize( player.origin - myOrigin )", "player.origin - myOrigin", 1),
            "observer_origin_zero": staged.replace("myOrigin = self.origin;", "myOrigin = ( 0, 0, 0 );", 1),
            "forward_from_origin": staged.replace("anglestoforward( myAngles )", "anglestoforward( myOrigin )", 1),
        }
        rejected = []
        for name, mutation in mutations.items():
            assert mutation != staged
            try:
                verify_staged_source(baseline, mutation, utility)
            except AssertionError:
                rejected.append(name)
            else:
                raise AssertionError(("Source contract accepted mutation", name))
            rows = [dict(id=1, origin=(14, 23, 35), sight=(True, False, False),
                         bullet=("pass", "blocked", "blocked"))]
            expected = Runtime(baseline_math, rows, geometry=True,
                               observer_origin=(13, 3, -2), observer_angles=(17, 91, 0))
            mutated = Runtime(compile_functions(mutation, utility), rows, geometry=True,
                              observer_origin=(13, 3, -2), observer_angles=(17, 91, 0))
            expected.run(); mutated.run()
            try:
                verify_staged_runtime(expected, mutated, ("runtime_mutation", name))
            except AssertionError:
                pass
            else:
                raise AssertionError(("Runtime contract accepted mutation", name))
        staged_report = dict(passed=True, cases=staged_count, modeled_vector_cases=staged_geometry_count,
            rejected_mutations=rejected, forward_calls_saved=forward_saved,
            origin_field_reads_saved=origin_reads_saved,
            scope="Actual parsed helper and candidate expression; exact modeled normalize/dot operands/results, state/RNG/distance/LOS order. Modeled Python vectors do not attest native floating point or collision geometry.",
            no_candidate_cost="The candidate unconditionally computes one forward and reads one origin; an empty/invalid/script-only decision previously computed no candidate forward.")

    paths = [old_path, new_path, utility_path]
    if staged_patch is not None:
        paths.append(staged_patch)
    return dict(passed=True, cases=count, native_calls_avoided=reductions,
                staged_math_hoist=staged_report,
                source_sha256={str(path.relative_to(project)): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths},
                scope="Actual parsed target-loop/helper statements with controlled native answers; modeled cone operands do not prove native FP, geometry, VM compiler, gameplay timing, or native match parity")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--staged-patch", type=Path,
                        help="Reviewed four-hunk hoist patch (default: staged/bots/target-loop-forward-origin.patch)")
    parser.add_argument("--no-staged-hoist", action="store_true", help="Run only the earlier visibility regression")
    args = parser.parse_args()
    started = time.perf_counter()
    project = args.project.resolve()
    patch = args.staged_patch or project / "staged/bots/target-loop-forward-origin.patch"
    report = regression(project, None if args.no_staged_hoist else patch.resolve())
    report["elapsed_seconds"] = time.perf_counter() - started
    report["tool_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    value = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(value, encoding="utf-8")
    print(value, end="")


if __name__ == "__main__":
    main()
