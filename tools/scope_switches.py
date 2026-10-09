"""Scope terminating case bodies accepted by MWCC but rejected by Clang.

Only used on the explicitly audited file list. Fall-through bodies are left
alone. Compiler diagnostics remain the gate for references crossing a case.
"""
import re

TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|::|[A-Za-z_]\w*|[^\s]', re.S)


def scope_switches(source):
    # Ignore directive text while retaining offsets into the original source.
    # Branches with different brace structure still require manual adaptation.
    lexical = re.sub(r'(?m)^[ \t]*#[^\n]*', lambda m: ' ' * len(m[0]), source)
    tokens = [m for m in TOKEN.finditer(lexical) if not m.group().startswith(("//", "/*"))]
    depth, stack, level, closes = [], [], 0, {}
    for i, token in enumerate(tokens):
        value = token.group()
        if value == "}":
            level -= 1
            if stack:
                closes[stack.pop()] = i
        depth.append(level)
        if value == "{":
            stack.append(i)
            level += 1
    edits = []
    for i, token in enumerate(tokens):
        if token.group() != "switch" or i+1 >= len(tokens) or tokens[i+1].group() != "(":
            continue
        j, parentheses = i+1, 0
        while j < len(tokens):
            if tokens[j].group() == "(": parentheses += 1
            elif tokens[j].group() == ")":
                parentheses -= 1
                if not parentheses: break
            j += 1
        opening = j+1
        if opening not in closes: continue
        closing = closes[opening]
        body_depth = depth[opening]+1
        labels = [n for n in range(opening+1, closing)
                  if depth[n] == body_depth and tokens[n].group() in ("case", "default")]
        for index, label in enumerate(labels):
            end = labels[index+1] if index+1 < len(labels) else closing
            colon = next((n for n in range(label, end) if tokens[n].group() == ":"), None)
            if colon is None or colon+1 == end: continue
            start = colon+1
            if tokens[start].group() == "{" and closes.get(start) == end-1: continue
            top = [n for n in range(start, end) if depth[n] == body_depth]
            if not top or tokens[top[-1]].group() != ";": continue
            # The last top-level statement must terminate this case. Do not
            # introduce a scope around intentional fall-through declarations.
            boundary = max((k for k,n in enumerate(top[:-1]) if tokens[n].group() in (";", "}")), default=-1)
            statement = [tokens[n].group() for n in top[boundary+1:]]
            if not statement or statement[0] not in ("break", "return", "continue", "goto"): continue
            edits.append((tokens[colon].end(), " {"))
            next_pos = tokens[end].start()
            line_start = source.rfind("\n", 0, next_pos)+1
            indent = source[line_start:next_pos]
            if indent.strip():
                edits.append((next_pos, " } "))
            else:
                edits.append((line_start, indent+"}\n"))
    for position, text in sorted(edits, reverse=True):
        source = source[:position]+text+source[position:]
    return source
