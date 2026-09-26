#!/usr/bin/env python3
"""Ordinal-prefix gate for a consumer's manual copy of rehlds_api.h.

rehlds_api.h exists three times: this repo (authoritative), KTP-ReAPI (a byte
mirror, gated by diff) and KTPAMXX/public/resdk/engine (a deliberate PREFIX,
compiled into both shipped KTPAMXX artifacts including dodx). The prefix copy
cannot be byte-diffed, and it is the one a consumer binds to by INDEX: a call
lands on vtable slot N / struct offset N chosen at compile time, so a slot that
moved is a wrong-function call with no load error, no crash and no diagnostic.

What this enforces, per ordered ABI surface:

  strict  the consumer's member-name sequence is an exact in-order PREFIX of
          the engine's -- consumer[i] == engine[i] for every i it declares.
          Rejects an insert, a reorder and a deletion at the index it happens.
  strict  same parameter arity at each shared index. Arity survives the typedef
          spellings the copies legitimately disagree on (`struct usercmd_s *`
          vs `usercmd_t *`), so it is the part of the signature worth failing on.
  strict  the consumer is never LONGER than the engine.
  notice  remaining signature-text differences, reported but not failed --
          spelling drift is expected; arity drift above is not.

A shorter consumer is allowed because slots are append-only at the engine's
tail, which is why plain equality would be a false red today. "Shorter is fine"
on its own would also pass a parse that fell over, so every surface carries
controls: it must be found on both sides, its body must be CLOSED, both member
lists must be non-empty, and --require names must be found at an equal index.
--selftest exercises the mutations this is meant to catch.

Matching is fixed-string (index/startswith) or a literal compiled regex; no
pattern is ever built from an argument.
"""

from __future__ import annotations

import argparse
import sys

# (surface label, the line that opens it) -- matched with startswith, in order.
SURFACES = [
    ("IRehldsHookchains (vtable)", "class IRehldsHookchains"),
    ("RehldsFuncs_t (function-pointer struct)", "struct RehldsFuncs_t"),
    ("IRehldsApi (vtable)", "class IRehldsApi"),
]

BODY_END = "};"
NEGATIVE_CONTROL = "__ktp_drift_negative_control__"


class ParseError(Exception):
    pass


def read_lines(path: str) -> list[str]:
    # The three repos' .gitattributes materialize different eols on a Linux
    # runner (this repo verbatim CRLF, KTPAMXX native LF), so the sibling
    # byte-diff passes on a coincidence that does not hold here.
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        return fh.read().replace("\r\n", "\n").replace("\r", "\n").split("\n")


def strip_comments(lines: list[str]) -> list[str]:
    out, in_block = [], False
    for line in lines:
        if in_block:
            end = line.find("*/")
            if end < 0:
                out.append("")
                continue
            line, in_block = line[end + 2 :], False
        start = line.find("/*")
        while start >= 0:
            end = line.find("*/", start + 2)
            if end < 0:
                line, in_block = line[:start], True
                break
            line = line[:start] + " " + line[end + 2 :]
            start = line.find("/*")
        cpp = line.find("//")
        if cpp >= 0:
            line = line[:cpp]
        out.append(line)
    return out


def body_of(lines: list[str], opener: str, where: str) -> list[str]:
    start = None
    for i, line in enumerate(lines):
        if line.lstrip().startswith(opener):
            start = i
            break
    if start is None:
        raise ParseError(f"{where}: `{opener}` not found -- dead probe, not agreement")
    for j in range(start + 1, len(lines)):
        if lines[j].startswith(BODY_END):
            return lines[start + 1 : j]
    raise ParseError(
        f"{where}: `{opener}` is never closed by a column-0 `{BODY_END}` -- "
        "the file is truncated or the body is malformed, which a prefix rule "
        "would otherwise swallow as 'shorter'"
    )


def split_params(text: str) -> int:
    """Top-level comma count + 1, or 0 for an empty list."""
    inner = text.strip()
    if not inner or inner == "void":
        return 0
    depth, commas = 0, 0
    for ch in inner:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            commas += 1
    return commas + 1


def paren_span(text: str, open_at: int) -> tuple[int, int]:
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return open_at + 1, i
    raise ParseError(f"unbalanced parentheses in: {text.strip()!r}")


IDENT_OK = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_~")


def members(body: list[str], opener: str, where: str) -> list[tuple[str, int, str]]:
    """[(name, arity, normalized signature)] in declaration order."""
    out = []
    # Declarations wrap in neither copy; one member per line holds for all three
    # surfaces, and a wrapped one would surface as a parse error, not a pass.
    for line in body:
        text = line.strip()
        if not text:
            continue
        if opener.startswith("struct"):
            # `void(*MSG_WriteCoord)(sizebuf_t *sb, float f);`
            at = text.find("(*")
            if at < 0:
                continue
            close = text.find(")", at + 2)
            if close < 0:
                raise ParseError(f"{where}: cannot read a member name from {text!r}")
            name = text[at + 2 : close]
            popen = text.find("(", close + 1)
            if popen < 0:
                raise ParseError(f"{where}: no parameter list on {text!r}")
        else:
            if not text.startswith("virtual "):
                continue
            popen = text.find("(")
            if popen < 0:
                raise ParseError(f"{where}: no parameter list on {text!r}")
            end = popen
            while end > 0 and text[end - 1] == " ":
                end -= 1
            begin = end
            while begin > 0 and text[begin - 1] in IDENT_OK:
                begin -= 1
            name = text[begin:end]
        if not name:
            raise ParseError(f"{where}: empty member name from {text!r}")
        lo, hi = paren_span(text, popen)
        out.append((name, split_params(text[lo:hi]), " ".join(text.split())))
    return out


def compare(engine: list, consumer: list, label: str, required: list[str]) -> list[str]:
    errs, e_n, c_n = [], len(engine), len(consumer)
    if e_n == 0 or c_n == 0:
        return [f"{label}: extracted {c_n} consumer / {e_n} engine members -- dead probe"]
    if c_n > e_n:
        errs.append(
            f"{label}: the consumer declares {c_n} members but the engine has only "
            f"{e_n} -- the consumer is AHEAD of the engine it compiles against"
        )
    for i in range(min(e_n, c_n)):
        en, ea, _ = engine[i]
        cn, ca, _ = consumer[i]
        if en != cn:
            errs.append(
                f"{label}: slot {i} is `{cn}` in the consumer and `{en}` in the "
                "engine -- NOT an in-order prefix. Slots are APPEND-ONLY at the "
                "engine's tail; a mid-list insert or reorder rebinds every later "
                "slot to the wrong function, silently, at runtime"
            )
            break
        if ea != ca:
            errs.append(
                f"{label}: slot {i} `{en}` takes {ea} parameter(s) in the engine "
                f"and {ca} in the consumer"
            )
    for name in required:
        # Fixed-string membership, so a name with regex metacharacters cannot
        # quietly become a pattern that matches everything or nothing.
        e_idx = [i for i, m in enumerate(engine) if m[0] == name]
        if not e_idx:
            continue  # another surface owns it; run() proves some surface does
        c_idx = [i for i, m in enumerate(consumer) if m[0] == name]
        if not c_idx:
            # Absent is only innocent if it sits in the tail the consumer has
            # not picked up yet. Anywhere else it is a deletion.
            if e_idx[0] < c_n:
                errs.append(
                    f"{label}: control `{name}` is engine slot {e_idx[0]}, inside "
                    f"the {c_n} slots the consumer declares, but the consumer does "
                    "not declare it"
                )
            continue
        if e_idx[0] != c_idx[0]:
            errs.append(
                f"{label}: control `{name}` is slot {c_idx[0]} in the consumer and "
                f"slot {e_idx[0]} in the engine"
            )
    return errs


def surface_notices(engine: list, consumer: list, label: str) -> list[str]:
    out = []
    for i in range(min(len(engine), len(consumer))):
        if engine[i][0] == consumer[i][0] and engine[i][2] != consumer[i][2]:
            out.append(f"{label}: slot {i} `{engine[i][0]}` differs in signature text")
    return out


def run(engine_path: str, consumer_path: str, required: list[str]) -> int:
    engine_lines = strip_comments(read_lines(engine_path))
    consumer_lines = strip_comments(read_lines(consumer_path))

    errors, notices, summary, seen = [], [], [], set()
    for label, opener in SURFACES:
        try:
            e_mem = members(body_of(engine_lines, opener, engine_path),
                            opener, engine_path)
        except ParseError as exc:
            errors.append(str(exc))
            continue
        # Engine-side names are collected before the consumer is parsed, so a
        # broken consumer cannot also make every control look stale.
        seen.update(m[0] for m in e_mem)
        try:
            c_mem = members(body_of(consumer_lines, opener, consumer_path),
                            opener, consumer_path)
        except ParseError as exc:
            errors.append(str(exc))
            continue
        errors.extend(compare(e_mem, c_mem, label, required))
        notices.extend(surface_notices(e_mem, c_mem, label))
        lag = [m[0] for m in e_mem[len(c_mem) :]]
        summary.append(
            f"  {label}: consumer {len(c_mem)} / engine {len(e_mem)}"
            + (f" -- consumer lags by tail: {', '.join(lag)}" if lag else " -- level")
        )

    # Controls on the controls. A --require name no surface owns is a stale
    # control that asserts nothing, and the negative one proves the matcher can
    # still say no -- without both, a broken matcher reads as a clean pass.
    for name in required:
        if name not in seen:
            errors.append(
                f"control `{name}` is on no engine surface -- the control is stale "
                "and was asserting nothing"
            )
    if NEGATIVE_CONTROL in seen:
        errors.append("negative control matched -- the matcher is broken")

    print(f"engine   : {engine_path}")
    print(f"consumer : {consumer_path}")
    print("\n".join(summary))
    for note in notices:
        print(f"::notice::{note} (expected where a typedef and its struct tag "
              "disagree; arity is checked strictly above)")
    if errors:
        for err in errors:
            print(f"::error::{err}")
        return 1
    print("OK: every ordered surface in the consumer copy is an exact in-order "
          "prefix of the engine's, with matching arity.")
    return 0


# --- selftest ---------------------------------------------------------------

_BASE = """#pragma once
class IRehldsHookchains {
public:
\tvirtual ~IRehldsHookchains() { }
\tvirtual IRehldsHookRegistry_A* A() = 0;
\tvirtual IRehldsHookRegistry_B* B() = 0;
\tvirtual IRehldsHookRegistry_SV_UpdatePausedHUD* SV_UpdatePausedHUD() = 0;
\tvirtual IRehldsHookRegistry_C* C() = 0;
};
struct RehldsFuncs_t {
\tvoid(*F1)(int a);
\tvoid(*F2)(sizebuf_t *sb, float f);
\tvoid(*SetServerPause)(bool status);
};
class IRehldsApi {
public:
\tvirtual ~IRehldsApi() { }
\tvirtual int GetMajorVersion() = 0;
};
"""

_CASES = [
    ("identical", _BASE, 0),
    ("valid prefix (tail dropped)",
     _BASE.replace("\tvirtual IRehldsHookRegistry_C* C() = 0;\n", "")
          .replace("\tvoid(*SetServerPause)(bool status);\n", ""), 0),
    ("mid-vtable insert",
     _BASE.replace("\tvirtual IRehldsHookRegistry_B* B() = 0;\n",
                   "\tvirtual IRehldsHookRegistry_B* B() = 0;\n"
                   "\tvirtual IRehldsHookRegistry_X* X() = 0;\n"), 1),
    ("mid-vtable deletion",
     _BASE.replace("\tvirtual IRehldsHookRegistry_B* B() = 0;\n", ""), 1),
    ("reorder",
     _BASE.replace("\tvirtual IRehldsHookRegistry_A* A() = 0;\n"
                   "\tvirtual IRehldsHookRegistry_B* B() = 0;\n",
                   "\tvirtual IRehldsHookRegistry_B* B() = 0;\n"
                   "\tvirtual IRehldsHookRegistry_A* A() = 0;\n"), 1),
    ("mid-struct insert in RehldsFuncs_t",
     _BASE.replace("\tvoid(*F2)(sizebuf_t *sb, float f);\n",
                   "\tvoid(*Fx)(int q);\n\tvoid(*F2)(sizebuf_t *sb, float f);\n"), 1),
    ("arity change at a shared slot",
     _BASE.replace("\tvoid(*F2)(sizebuf_t *sb, float f);",
                   "\tvoid(*F2)(sizebuf_t *sb, float f, int extra);"), 1),
    ("consumer ahead of engine",
     _BASE.replace("\tvirtual IRehldsHookRegistry_C* C() = 0;\n",
                   "\tvirtual IRehldsHookRegistry_C* C() = 0;\n"
                   "\tvirtual IRehldsHookRegistry_D* D() = 0;\n"), 1),
    ("truncated mid-list (body never closed)",
     _BASE.split("\tvirtual IRehldsHookRegistry_C* C() = 0;")[0], 1),
    ("surface missing entirely",
     _BASE.replace("struct RehldsFuncs_t", "struct RehldsFuncs_renamed_t"), 1),
    ("signature spelling only (notice, not failure)",
     _BASE.replace("void(*F2)(sizebuf_t *sb, float f);",
                   "void(*F2)(struct sizebuf_s *sb, float f);"), 0),
    ("stale control name (asserts nothing)", _BASE, 1, ["NoSuchMember"]),
    ("control present in engine, dropped from the consumer's prefix",
     _BASE.replace("\tvirtual IRehldsHookRegistry_SV_UpdatePausedHUD*"
                   " SV_UpdatePausedHUD() = 0;\n", ""), 1),
]


def selftest(tmpdir: str) -> int:
    import os

    engine = os.path.join(tmpdir, "engine.h")
    consumer = os.path.join(tmpdir, "consumer.h")
    with open(engine, "w", encoding="utf-8", newline="\r\n") as fh:
        fh.write(_BASE)  # CRLF on purpose: the engine copy is stored that way
    failures = 0
    for case in _CASES:
        name, text, expected = case[0], case[1], case[2]
        required = case[3] if len(case) > 3 else ["SV_UpdatePausedHUD", "F2"]
        with open(consumer, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        got = run(engine, consumer, required)
        verdict = "ok" if got == expected else "SELFTEST FAILURE"
        if got != expected:
            failures += 1
        print(f"--- selftest [{verdict}] {name}: expected {expected}, got {got}\n")
    if failures:
        print(f"::error::{failures} selftest case(s) did not behave as declared")
        return 1
    print(f"selftest: all {len(_CASES)} cases behaved as declared")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--engine", help="authoritative rehlds_api.h")
    ap.add_argument("--consumer", help="the prefix copy to check")
    ap.add_argument("--require", action="append", default=[],
                    help="member name that must sit at the same index in both")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            return selftest(tmp)
    if not args.engine or not args.consumer:
        ap.error("--engine and --consumer are required unless --selftest")
    return run(args.engine, args.consumer, args.require)


if __name__ == "__main__":
    sys.exit(main())
