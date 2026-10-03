"""Staged call/declaration distinction for capabilities.CALL_RE matches.

This does not scan sources or modify the production census. Review and integrate
the predicate immediately after balanced() in census(), before registration-kind
dispatch. Other SDK/field declaration inventories remain separate and unchanged.
"""
import re


FORMAL_TYPE = re.compile(r'^(?:(?:const|volatile)\s+)*(?:void|char|int|unsigned|signed|short|long|float|double|bool|byte|DWORD|XAssetType|struct|union|enum)\b')
EXPRESSION_WORD = re.compile(r'\b(?:return|co_return|throw|if|else|while|for|switch|case|sizeof|alignof|new|delete|do|goto)\b')
TYPE_HEAD = re.compile(r'(?:[A-Za-z_]\w*|::|\*|&|\s)+$')


def _strip_attributes(head):
    """Remove known attribute invocations without guessing their contents."""
    attribute = re.compile(r'\b(?:__declspec|__attribute__|alignas)\s*\(')
    while match := attribute.search(head):
        cursor, depth = match.end(), 1
        while cursor < len(head) and depth:
            depth += (head[cursor] == '(') - (head[cursor] == ')')
            cursor += 1
        if depth:
            return None
        head = head[:match.start()] + ' ' + head[cursor:]
    return head


def registration_is_declaration(masked, start, end, args):
    """Exclude confident prototypes/definitions; retain uncertain call sites.

    `masked` must preserve the offsets of the comment/inactive/literal-masked
    source. `start` is CALL_RE.start(), `end` is balanced()'s closing-paren end,
    and args are arguments(body). A call assignment is never a declaration.
    This is deliberately narrower than a C/C++ parser; unknown syntax is kept
    as inventory for manual review, rather than silently dropped.
    """
    tail = masked[end:].lstrip()
    if tail.startswith('{'):
        return True
    first = args[0].strip() if args else ''
    # Formal type keywords cannot be registration-name expressions. Include
    # void: it is the exact reason six no-argument prototypes were miscounted.
    if first and FORMAL_TYPE.match(first):
        return True
    before = masked[:start]
    boundary = max(before.rfind(';'), before.rfind('{'), before.rfind('}')) + 1
    head = re.sub(r'^[\t ]*#[^\n]*', '', before[boundary:], flags=re.M).strip()
    if not head or EXPRESSION_WORD.search(head) or '=' in head:
        return False
    head = _strip_attributes(head)
    if head is None:
        return False
    # A return type, qualifiers, calling convention, namespace, and pointers
    # form a declaration head. Operators/commas/parentheses make it uncertain.
    if not TYPE_HEAD.fullmatch(head) or not re.search(r'[A-Za-z_]\w*', head):
        return False
    if tail.startswith(';'):
        return True
    suffix = _strip_attributes(tail)
    return suffix is not None and suffix.lstrip().startswith(';')


INTEGRATION = '''
body, end = balanced(clean, match.end() - 1)
args = arguments(body)
if registration_is_declaration(masked, match.start(), end, args):
    continue
'''
