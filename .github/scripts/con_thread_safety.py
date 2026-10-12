#!/usr/bin/env python3
"""Source gate for the console path the Steam background thread reaches.

`sv_steam3.cpp` has carried the rule "the Steam thread must never reach
Con_Printf" since .913, and 34 lines below it the thread's send loop called
NET_SendPacket, whose `ca_dedicated` failure branch in `net_ws.cpp` logs through
exactly that path. The comment was true, documented, and violated by the same
file pair for eighteen builds. .931 made the path safe instead of discarding the
diagnostic; nothing made the rule FAIL when it is broken, which is why a comment
was not enough the first time.

So each rule below is the invariant itself, not a restatement of it. A rule
fires on the shape of the code, and `--selftest` mutates the real sources and
asserts every mutation is rejected -- a gate nobody has watched fail is not a
gate, and that standard applies per assertion rather than per rule.

What is enforced:

  Con_DebugLog        the format buffer `Q_vsnprintf` writes into is declared
                      inside the body (a shared static one was the first race);
                      the handle is std::atomic, published with a release store
                      of the freshly opened handle and read with an acquire
                      load, and is never closed or un-published -- closing it
                      is the use-after-free the design forbids, and a
                      close-on-write-failure "fix" is its likeliest shape;
                      no FS_Open-family call is reached without a
                      KTP_IsGameThread() check that RETURNS ahead of it --
                      opening walks the search paths and mutates the
                      filesystem's open-file list, writing does not;
                      every off-thread refusal increments g_ktp_condbg_drops,
                      because a drop with no counter reads as no drop;
                      the write through the cached handle is tested and a
                      failure counted;
                      no FS_GetLocalPath + raw fopen -- FS_Open with "at" takes
                      the first WRITABLE search path, FS_GetLocalPath the first
                      one a stat hits, pack files included, so the swap
                      silently moves the file HLStatsX tails.
  Con_Printf_internal the rcon-redirect capture into outputbuf stays gated on
                      KTP_IsGameThread(), in whatever order the condition is
                      written.
  sv_main.cpp         g_ktp_conio_* stay atomic AT THEIR DEFINITION, and
                      condbg_drops is actually printed on the [KTP_PROFILE] io:
                      line from that counter -- one nobody emits is worth no
                      more than no counter.
  sv_steam3.cpp       no console call in any body the Steam thread reaches --
                      the thread function, the refresh publisher, the callback
                      enqueue, and the `On*` handlers the file itself documents
                      as "called from background thread via Steam API". The
                      .913 bug was a violation one frame DOWN from the comment,
                      so a rule that reads only the thread body mechanizes the
                      one shape of the bug that did not happen. `Sys_Printf` and
                      `Con_SafePrintf` are banned alongside `Con_Printf`:
                      Con_Printf_internal funnels into the first, so banning
                      only the wrapper is a one-token bypass.
                      And the NET_SendPacket call still carries the note naming
                      the one path that is a deliberate exception.

Controls, so a blind run cannot pass: every file must be found and clear a size
floor, every function body must be located and CLOSED, each body must be
non-trivial, the handle-access and refusal counts must be plausible, and the
body finder must REFUSE a name that is not there. A rule with nothing to judge
raises rather than passing, and every raise exits 2 -- so a dead probe is never
mistaken for a clean tree. Any unexpected exception exits 2 for the same reason.

What this cannot see, stated rather than implied: it reads shape, not control
flow. A thread check moved into a branch that does not dominate the open it
guards still satisfies the ordering rule, and a console call added to a
background-reachable function in ANOTHER translation unit is out of scope.
Two refactors it will red although they are correct, so the message says what to
do rather than leaving you guessing: factoring the four identical drop
increments into a helper (the rule wants the counter named at the refusal), and
hoisting the thread check into a named local (there it exits 2 and says the rule
needs updating rather than the code). Fix the gate in that case, not the code.
And a red run only blocks a merge once the job is a required status check on
`main`; until then it informs.

Matching is fixed-string or a literal compiled regex; no pattern is ever built
from an argument.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
import tempfile

SYS_DLL = "rehlds/engine/sys_dll.cpp"
SV_STEAM3 = "rehlds/engine/sv_steam3.cpp"
SV_MAIN = "rehlds/engine/sv_main.cpp"
NET_WS = "rehlds/engine/net_ws.cpp"

FILES = [SYS_DLL, SV_STEAM3, SV_MAIN, NET_WS]

# A floor, not a measurement: any of these files truncated to this size is a
# broken checkout or a wrong path, not a tree that satisfies every rule.
SIZE_FLOOR = 20000

# Not a control -- a paste guard. The real discrimination control is
# rule_control_body_finder_discriminates, which asserts the body finder REFUSES
# a name that is not in the file.
PASTE_GUARD = "__ktp_conthread_negative_control__"

CONDEBUGLOG = "Con_DebugLog(const char *file"
CONPRINTF_INTERNAL = "Con_Printf_internal(const char *Dest)"

# Every body in sv_steam3.cpp the Steam thread can reach. Steam_ThreadFunc runs
# on it; SteamRefresh_ProcessIfPending and the On* handlers are called from it
# (the latter via SteamGameServer_RunCallbacks, and the file's own banner at the
# On* block says "called from background thread via Steam API");
# SteamCallbackQueue_Push is called from those handlers. The Process* twins are
# deliberately absent -- they run on the game thread and print there by design.
STEAM_THREAD_BODIES = [
    "Steam_ThreadFunc(",
    "SteamRefresh_ProcessIfPending()",
    "SteamCallbackQueue_Push(const QueuedCallback",
    "CSteam3Server::OnGSPolicyResponse(",
    "CSteam3Server::OnLogonSuccess(",
    "CSteam3Server::OnLogonFailure(",
    "CSteam3Server::OnGSClientDeny(",
    "CSteam3Server::OnGSClientDenyHelper(",
    "CSteam3Server::OnGSClientApprove(",
    "CSteam3Server::OnGSClientKick(",
]

BANNED_CONSOLE = ("Con_Printf(", "Con_DPrintf(", "Con_DebugLog(",
                  "Con_SafePrintf(", "Sys_Printf(")

GAME_THREAD_CHECK = "KTP_IsGameThread()"
DROP_COUNTER = "g_ktp_condbg_drops.fetch_add"
HANDLES = ("s_condbgFh", "s_condbgFd")

# A refusal is `if (!KTP_IsGameThread()) { drops++; return; }`. Measured in
# non-blank CODE lines after stripping, so a three-line explanatory comment
# inside the block -- which this file's own refusals carry -- costs nothing.
DROP_WINDOW = 4

_STRIP_CHAR = re.compile(r"'(?:\\.|[^'\\])*'")
_STRIP_STRING = re.compile(r'"(?:\\.|[^"\\])*"')
_OPEN_CALL = re.compile(r"\b(?:FS_Open\w*|_open|open)\s*\(")
_CLOSE_CALL = re.compile(r"\b(?:FS_Close|_close|close)\s*\(\s*(\w+)\s*\)")
_STATIC_DECL = re.compile(r"(?:^\s*|[;{}]\s*)static\b")
_IF_ZERO = re.compile(r"^#\s*if\s+(?:0|false)\s*$", re.IGNORECASE)
_IF_ANY = re.compile(r"^#\s*if")
_ELSE_ANY = re.compile(r"^#\s*el(?:se|if)")
_ENDIF = re.compile(r"^#\s*endif")
_VSNPRINTF = re.compile(r"\bQ_vsnprintf\s*\(\s*(\w+)\s*,")
_REFUSAL = re.compile(r"if\s*\(\s*!\s*KTP_IsGameThread\s*\(\s*\)\s*\)")


class GateError(Exception):
    """A control failed: the gate cannot see what it claims to judge."""


def read_lines(path: str) -> list[str]:
    # This repo stores engine sources CRLF (.gitattributes pins only *.sh to
    # LF), so a checkout on either platform must normalize before matching.
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        text = fh.read()
    return text.replace("\r\n", "\n").replace("\r", "\n").split("\n")


def strip_comments(lines: list[str]) -> list[str]:
    """Comments removed, STATEFULLY, so a `/* */` spanning lines is not code.

    Same algorithm as rehlds_api_prefix.py in this directory. A per-line regex
    was tried first and is wrong in the dangerous direction: an unbalanced brace
    inside a multi-line comment closes a function body early, every rule below
    then judges a prefix of the function, and the gate reports that all
    invariants hold over code it never read.
    """
    out, in_block = [], False
    for line in lines:
        if in_block:
            end = line.find("*/")
            if end < 0:
                out.append("")
                continue
            line, in_block = line[end + 2:], False
        start = line.find("/*")
        while start >= 0:
            end = line.find("*/", start + 2)
            if end < 0:
                line, in_block = line[:start], True
                break
            line = line[:start] + " " + line[end + 2:]
            start = line.find("/*")
        cpp = line.find("//")
        if cpp >= 0:
            line = line[:cpp]
        out.append(line)
    return out


def strip_if_zero(lines: list[str]) -> list[str]:
    """Blank the body of `#if 0` / `#if false`, keeping its `#else` arm live.

    A Con_Printf parked in a disabled block is not a violation, and failing on
    it teaches people to work around the gate. Nested `#if`s inside a dead block
    stay dead; a dead block with unbalanced braces makes a body look unclosed,
    which raises rather than passes.
    """
    out, depth = [], None
    for line in lines:
        s = line.strip()
        if depth is None:
            if _IF_ZERO.match(s):
                depth = 0
                out.append("")
                continue
            out.append(line)
            continue
        if _IF_ANY.match(s):
            depth += 1
        elif depth == 0 and (_ELSE_ANY.match(s) or _ENDIF.match(s)):
            depth = None
        elif _ENDIF.match(s):
            depth -= 1
        out.append("")
    return out


def code_lines(lines: list[str]) -> list[str]:
    """Comments, dead `#if 0` blocks, strings and char literals removed.

    Line numbering is preserved throughout, so every offset a rule reports is
    the real file's line.
    """
    out = []
    for line in strip_if_zero(strip_comments(lines)):
        line = _STRIP_STRING.sub('""', line)
        out.append(_STRIP_CHAR.sub("''", line))
    return out


def statement_at(code: list[str], n: int, span: int = 4) -> str:
    """Line `n` joined forward to its `;`, `{` or `}`, so a wrapped call matches.

    Without this, an 80-column wrap of `s_condbgFh.store(fh,` / newline /
    `std::memory_order_release);` reads as a store with no memory order.
    """
    parts = []
    for k in range(n, min(n + span, len(code))):
        parts.append(code[k])
        if re.search(r"[;{}]", code[k]):
            break
    return " ".join(x.strip() for x in parts)


def find_bodies(code: list[str], needle: str, label: str) -> list[tuple[int, int]]:
    """Every [start, end) body of a definition containing `needle`.

    A definition is a line containing `needle` whose next non-blank,
    non-preprocessor line opens a block -- so a declaration ending in `;` and a
    call site are both excluded. ALL of them are returned, not the last: a
    function split into two complete `#ifdef _WIN32` / `#else` definitions has
    two bodies, and judging one of them is a silent pass on the other.
    """
    openers = []
    for i, line in enumerate(code):
        if needle not in line:
            continue
        j = i + 1
        while j < len(code) and (not code[j].strip() or code[j].lstrip().startswith("#")):
            j += 1
        if j < len(code) and code[j].strip().startswith("{"):
            openers.append(j)
    if not openers:
        raise GateError(f"{label}: no definition of {needle!r} found -- dead probe")

    bodies = []
    for start in sorted(set(openers)):
        depth = 0
        for i in range(start, len(code)):
            depth += code[i].count("{") - code[i].count("}")
            if depth == 0 and i > start:
                bodies.append((start + 1, i))
                break
        else:
            raise GateError(
                f"{label}: body of {needle!r} opening at line {start + 1} is never closed "
                "-- truncated source or an unbalanced preprocessor arm"
            )
    return bodies


class Gate:
    def __init__(self, root: str) -> None:
        self.root = root
        self.failures: list[str] = []
        self.notices: list[str] = []
        self.raw: dict[str, list[str]] = {}
        self.code: dict[str, list[str]] = {}
        for rel in FILES:
            path = os.path.join(root, rel.replace("/", os.sep))
            if not os.path.isfile(path):
                raise GateError(f"{rel}: not found under {root} -- dead probe")
            size = os.path.getsize(path)
            if size < SIZE_FLOOR:
                raise GateError(
                    f"{rel}: {size} bytes is below the {SIZE_FLOOR}-byte floor -- "
                    "truncated or wrong file, not a clean tree"
                )
            self.raw[rel] = read_lines(path)
            self.code[rel] = code_lines(self.raw[rel])
            if any(PASTE_GUARD in line for line in self.raw[rel]):
                raise GateError(f"{rel}: paste-guard token present in the source")

    def fail(self, rule: str, message: str) -> None:
        self.failures.append(f"{rule}: {message}")

    def bodies(self, rel: str, needle: str, floor: int = 10) -> list[tuple[list[str], int]]:
        """Code lines of each body plus its 0-based line offset."""
        out = []
        for start, end in find_bodies(self.code[rel], needle, rel):
            if end - start < floor:
                raise GateError(
                    f"{rel}: body of {needle!r} at line {start} is {end - start} lines, under the "
                    f"{floor}-line floor -- too small to be the real one"
                )
            out.append((self.code[rel][start:end], start))
        return out

    def one_body(self, rel: str, needle: str) -> tuple[list[str], int]:
        found = self.bodies(rel, needle)
        if len(found) != 1:
            raise GateError(
                f"{rel}: {needle!r} resolves to {len(found)} bodies, expected exactly one -- "
                "the rules below judge a single definition"
            )
        return found[0]

    # ---- controls ----------------------------------------------------------

    def rule_control_body_finder_discriminates(self) -> None:
        # The real negative control: the machinery every rule depends on must
        # REFUSE a name that is not in the file. Absence of a pasted token
        # proves nothing, because no rule searches for it.
        for rel in (SYS_DLL, SV_STEAM3):
            try:
                find_bodies(self.code[rel], PASTE_GUARD + "(", rel)
            except GateError:
                continue
            raise GateError(
                f"{rel}: the body finder returned a body for a name that is not in the file -- "
                "it is matching more than it means, so every verdict below is worthless"
            )

    # ---- Con_DebugLog ------------------------------------------------------

    def rule_format_buffer_is_local(self) -> None:
        rule = "condebuglog-format-buffer-is-local"
        body, offset = self.one_body(SYS_DLL, CONDEBUGLOG)
        joined = "\n".join(body)
        m = _VSNPRINTF.search(joined)
        if not m:
            raise GateError(
                f"{SYS_DLL}: no Q_vsnprintf in Con_DebugLog -- the rule would pass on nothing"
            )
        name = m.group(1)
        # Anchored to the buffer Q_vsnprintf actually writes into, not to the
        # absence of `static` in the body: moving the declaration to file scope
        # restores the shared buffer in full and empties the body of statics.
        decl = re.compile(r"\b(?:char|unsigned char)\s+" + re.escape(name) + r"\s*\[")
        found = [n for n, line in enumerate(body) if decl.search(line)]
        if not found:
            self.fail(
                rule,
                f"{SYS_DLL}: Con_DebugLog formats into `{name}`, which is not declared inside the "
                "body. A format buffer at file scope is shared by every thread that prints, which "
                "is the first of this function's three races.",
            )
        for n in found:
            if _STATIC_DECL.search(body[n]):
                self.fail(
                    rule,
                    f"{SYS_DLL}:{offset + n + 1} declares the format buffer `{name}` static. "
                    "It must be per-call, on the stack.",
                )
        for n, line in enumerate(body):
            if _STATIC_DECL.search(line):
                self.fail(
                    rule,
                    f"{SYS_DLL}:{offset + n + 1} declares `static` inside Con_DebugLog. The body "
                    "must hold no state two threads share.",
                )

    def rule_handle_is_atomic(self) -> None:
        rule = "condebuglog-handle-is-atomic"
        text = "\n".join(self.code[SYS_DLL])
        for decl in ("std::atomic<FileHandle_t> s_condbgFh", "std::atomic<int> s_condbgFd"):
            if decl not in text:
                self.fail(
                    rule,
                    f"{SYS_DLL}: `{decl}` is gone. The qconsole.log handle is read by the Steam "
                    "thread and must stay atomic; a plain static FILE* is the race.",
                )
        # Per access and per STATEMENT, not by the presence of the words in the
        # body: the _WIN32 and POSIX arms each publish their own handle, so one
        # arm's token would satisfy a whole-body search while the other had lost
        # its ordering -- and a wrapped call would read as having none at all.
        body, offset = self.one_body(SYS_DLL, CONDEBUGLOG)
        stores = loads = 0
        for n, line in enumerate(body):
            for handle in HANDLES:
                if f"{handle}.store(" in line:
                    stores += 1
                    stmt = statement_at(body, n)
                    if "memory_order_release" not in stmt:
                        self.fail(
                            rule,
                            f"{SYS_DLL}:{offset + n + 1} publishes {handle} without a release "
                            "store. The release store is what makes s_condbgName stable for "
                            "every other reader of the handle.",
                        )
                    # C1: the only legal publish is the handle just opened.
                    # `store(nullptr)` / `store(-1)` un-publishes a handle
                    # another thread may be writing through.
                    if not re.search(r"\.store\(\s*(?:fh|fd)\s*,", stmt):
                        self.fail(
                            rule,
                            f"{SYS_DLL}:{offset + n + 1} stores something other than the handle it "
                            "just opened into {handle}. The handle is published ONCE and never "
                            "un-published; clearing it is the use-after-free this design forbids, "
                            "because a reader already past the load is inside FS_FPrintf.".replace(
                                "{handle}", handle),
                        )
                if f"{handle}.load(" in line:
                    loads += 1
                    if "memory_order_acquire" not in statement_at(body, n):
                        self.fail(
                            rule,
                            f"{SYS_DLL}:{offset + n + 1} reads {handle} without an acquire load, "
                            "so a thread can see the handle before s_condbgName is written.",
                        )
        if stores < 2 or loads < 2:
            raise GateError(
                f"{SYS_DLL}: found {loads} loads and {stores} stores of the handle in "
                "Con_DebugLog, expected at least one of each per platform arm -- the rule would "
                "pass on nothing"
            )

    def rule_handle_is_never_closed(self) -> None:
        rule = "condebuglog-handle-is-never-closed"
        body, offset = self.one_body(SYS_DLL, CONDEBUGLOG)
        for n, line in enumerate(body):
            m = _CLOSE_CALL.search(line)
            if m and m.group(1) in ("fh", "fd"):
                self.fail(
                    rule,
                    f"{SYS_DLL}:{offset + n + 1} closes the cached handle `{m.group(1)}`. Closing "
                    "it is exactly 'one thread closes a handle another is writing through' -- and "
                    "closing on a write failure, so the next line reopens, is the likeliest shape "
                    "of that mistake. The second-destination path opens its own `once` handle for "
                    "this reason; do that instead.",
                )

    def rule_open_is_game_thread_only(self) -> None:
        rule = "condebuglog-open-is-game-thread-only"
        body, offset = self.one_body(SYS_DLL, CONDEBUGLOG)
        opens = [n for n, line in enumerate(body) if _OPEN_CALL.search(line)]
        if not opens:
            raise GateError(
                f"{SYS_DLL}: no open call found in Con_DebugLog -- the rule would pass on nothing"
            )
        refusals = self._refusals(body)
        for n in opens:
            guard = max((k for k in refusals if k < n), default=None)
            prev_open = max((k for k in opens if k < n), default=-1)
            if guard is None or guard < prev_open:
                self.fail(
                    rule,
                    f"{SYS_DLL}:{offset + n + 1} opens the log with no KTP_IsGameThread() refusal "
                    "ahead of it. FS_Open walks the search paths and mutates the filesystem's "
                    "open-file list; only the WRITE is safe off the game thread.",
                )

    def _refusals(self, body: list[str]) -> list[int]:
        """Lines holding an off-thread refusal that actually refuses.

        Whitespace-tolerant, and it requires the `return;`: keeping the test and
        the counter but deleting the return re-opens the off-thread path while
        satisfying every rule that only looks for the test.
        """
        out = []
        for n, line in enumerate(body):
            if not _REFUSAL.search(line):
                continue
            window, k = [], n
            while k < len(body) and len(window) <= DROP_WINDOW:
                if body[k].strip():
                    window.append(body[k])
                k += 1
            joined = " ".join(window)
            if "return" in joined:
                out.append(n)
        return out

    def rule_drops_are_counted(self) -> None:
        rule = "condebuglog-drop-is-counted"
        body, offset = self.one_body(SYS_DLL, CONDEBUGLOG)
        tests = [n for n, line in enumerate(body) if _REFUSAL.search(line)]
        if not tests:
            raise GateError(
                f"{SYS_DLL}: no off-thread test found in Con_DebugLog -- nothing to check. If the "
                "check was hoisted into a named local, this rule needs updating rather than the "
                "code"
            )
        refuses = set(self._refusals(body))
        for n in tests:
            window, k = [], n
            while k < len(body) and len(window) <= DROP_WINDOW:
                if body[k].strip():
                    window.append(body[k])
                k += 1
            joined = " ".join(window)
            if n not in refuses:
                self.fail(
                    rule,
                    f"{SYS_DLL}:{offset + n + 1} tests for the game thread but does not return, so "
                    "an off-thread caller falls through to the path the test exists to stop.",
                )
            if DROP_COUNTER not in joined:
                self.fail(
                    rule,
                    f"{SYS_DLL}:{offset + n + 1} refuses an off-thread line without incrementing "
                    "g_ktp_condbg_drops. A drop with no counter reads exactly like no drop, and a "
                    "qconsole.log that is never written must not report perfect health.",
                )
        # The persistent-handle write is the one that can fail silently. Checked
        # at the write itself: a bare `< 0` anywhere in the body is satisfied by
        # the open-failure tests, which are a different check entirely.
        writes = [n for n, line in enumerate(body) if "FS_FPrintf(fh" in line]
        if not writes:
            raise GateError(
                f"{SYS_DLL}: no FS_FPrintf through the cached handle in Con_DebugLog -- the rule "
                "would pass on nothing"
            )
        for n in writes:
            stmt = statement_at(body, n)
            window = " ".join(x for x in body[n:n + 3] if x.strip())
            if "< 0" not in stmt or DROP_COUNTER not in window:
                self.fail(
                    rule,
                    f"{SYS_DLL}:{offset + n + 1} writes through the cached handle without testing "
                    "the result and counting a failure, so a lost line is uncounted and a "
                    "qconsole.log nobody can write reports perfect health.",
                )

    def rule_no_localpath_fopen(self) -> None:
        rule = "condebuglog-no-localpath-fopen"
        body, offset = self.one_body(SYS_DLL, CONDEBUGLOG)
        for n, line in enumerate(body):
            for banned in ("FS_GetLocalPath", "fopen("):
                if banned in line:
                    self.fail(
                        rule,
                        f"{SYS_DLL}:{offset + n + 1} uses `{banned}`. FS_Open with \"at\" writes to "
                        "the first search path that ALLOWS WRITING; FS_GetLocalPath returns the "
                        "first one a stat hits, checking pack entries first -- so resolving the "
                        "path and opening it directly silently moves the file HLStatsX tails, or "
                        "hands back a path inside a .pak that fopen can never open.",
                    )

    # ---- Con_Printf_internal ----------------------------------------------

    def rule_redirect_capture_gated(self) -> None:
        rule = "conprintf-redirect-capture-gated"
        body, _ = self.one_body(SYS_DLL, CONPRINTF_INTERNAL)
        # Both tokens in one condition, in any order and with any comparison
        # spelling -- `sv_redirected != RD_NONE && KTP_IsGameThread()` is a
        # likely edit, since the line six above already spells it that way.
        for n, line in enumerate(body):
            if "sv_redirected" not in line:
                continue
            stmt = statement_at(body, n)
            if "if" in stmt and GAME_THREAD_CHECK in stmt:
                return
        self.fail(
            rule,
            f"{SYS_DLL}: no condition in Con_Printf_internal tests sv_redirected and "
            "KTP_IsGameThread() together, so the rcon-redirect capture is no longer gated on the "
            "game thread. outputbuf is unsynchronized; an off-thread print that takes the capture "
            "branch corrupts it.",
        )

    # ---- sv_main.cpp -------------------------------------------------------

    def rule_conio_counters_atomic(self) -> None:
        rule = "conio-counters-atomic"
        for name in ("g_ktp_conio_frame_us", "g_ktp_conio_worst_us"):
            # At the DEFINITION: an `extern std::atomic<uint32> <name>;`
            # satisfies a substring search while the real definition, wherever
            # it moved to, is unchecked.
            defn = re.compile(r"^\s*std::atomic<uint32>\s+" + re.escape(name) + r"\b")
            if not any(defn.match(line) for line in self.code[SV_MAIN]):
                self.fail(
                    rule,
                    f"{SV_MAIN}: `{name}` has no `std::atomic<uint32>` definition. Con_Printf runs "
                    "on background threads, so these counters tear if they are plain scalars -- "
                    "and an `extern` declaration is not a definition.",
                )

    def rule_drop_counter_is_emitted(self) -> None:
        rule = "condbg-drops-are-emitted"
        if "condbg_drops=%u" not in "\n".join(self.raw[SV_MAIN]):
            self.fail(
                rule,
                f"{SV_MAIN}: `condbg_drops=` is gone from the [KTP_PROFILE] io: line. A counter "
                "nobody prints is worth no more than no counter.",
            )
        if "g_ktp_condbg_drops.load" not in "\n".join(self.code[SV_MAIN]):
            self.fail(
                rule,
                f"{SV_MAIN}: the io: line no longer reads g_ktp_condbg_drops, so the field is "
                "printed from something else.",
            )

    # ---- sv_steam3.cpp -----------------------------------------------------

    def rule_steam_thread_no_direct_console(self) -> None:
        rule = "steam-thread-no-direct-console"
        for needle in STEAM_THREAD_BODIES:
            floor = 3 if needle != "Steam_ThreadFunc(" else 10
            for body, offset in self.bodies(SV_STEAM3, needle, floor=floor):
                for n, line in enumerate(body):
                    for banned in BANNED_CONSOLE:
                        if banned in line:
                            self.fail(
                                rule,
                                f"{SV_STEAM3}:{offset + n + 1} calls `{banned}` in "
                                f"`{needle.rstrip('(')}`, which the Steam thread reaches. That is "
                                "the rule stated at the top of this file; the one path that breaks "
                                "it reaches the console through NET_SendPacket's failure branch and "
                                "was made safe deliberately. Route a new diagnostic through the "
                                "async log ring, or enqueue it for the game thread the way the "
                                "callbacks already do.",
                            )

    def rule_steam_send_site_names_the_exception(self) -> None:
        rule = "steam-send-site-names-the-exception"
        total = 0
        for body, offset in self.bodies(SV_STEAM3, "Steam_ThreadFunc("):
            sends = [n for n, line in enumerate(body) if "NET_SendPacket(" in line]
            total += len(sends)
            raw = self.raw[SV_STEAM3]
            for n in sends:
                abs_n = offset + n
                window = "\n".join(raw[max(0, abs_n - 6):abs_n])
                if "Con_DebugLog" not in window:
                    self.fail(
                        rule,
                        f"{SV_STEAM3}:{abs_n + 1} sends from the Steam thread with no note above it "
                        "naming the console path a failed sendto() takes. That note is the only "
                        "thing telling the next reader why the rule above has an exception, and "
                        "the only place this gate's own existence is recorded in the source.",
                    )
        if not total:
            raise GateError(
                f"{SV_STEAM3}: no NET_SendPacket call in Steam_ThreadFunc -- the rule would pass "
                "on nothing"
            )

    def rule_notice_dedicated_log_path(self) -> None:
        # Not a failure: removing this Con_Printf would be a legitimate change.
        # But it is what makes every rule above load-bearing, so its state is
        # reported either way.
        probe = 'Con_Printf("%s: ERROR: %s'
        if probe in "\n".join(self.raw[NET_WS]):
            self.notices.append(
                f"{NET_WS}: the ca_dedicated send-failure Con_Printf is still present -- the "
                "Steam thread still reaches the console on a failed sendto(), so the rules above "
                "are live."
            )
        else:
            self.notices.append(
                f"{NET_WS}: the ca_dedicated send-failure Con_Printf is GONE. If that was "
                "deliberate, the off-thread console path may no longer exist and this gate's "
                "scope should be revisited."
            )

    NOTICE_ONLY = ("rule_notice_dedicated_log_path",)

    RULES = (
        rule_control_body_finder_discriminates,
        rule_format_buffer_is_local,
        rule_handle_is_atomic,
        rule_handle_is_never_closed,
        rule_open_is_game_thread_only,
        rule_drops_are_counted,
        rule_no_localpath_fopen,
        rule_redirect_capture_gated,
        rule_conio_counters_atomic,
        rule_drop_counter_is_emitted,
        rule_steam_thread_no_direct_console,
        rule_steam_send_site_names_the_exception,
        rule_notice_dedicated_log_path,
    )

    def run(self) -> int:
        for rule in self.RULES:
            rule(self)
        for notice in self.notices:
            print(f"notice: {notice}")
        if self.failures:
            for f in self.failures:
                print(f"::error::{f}")
            print(f"{len(self.failures)} cross-thread console invariant(s) violated")
            return 1
        judged = sum(1 for r in self.RULES if r.__name__ not in self.NOTICE_ONLY)
        print(f"all {judged} cross-thread console invariants hold")
        return 0


def run(root: str) -> int:
    try:
        return Gate(root).run()
    except GateError as exc:
        print(f"::error::control failed -- {exc}")
        return 2
    except Exception as exc:  # noqa: BLE001 -- see below
        # Deliberately 2, not 1. The whole point of this gate is that a probe
        # which could not look is never confusable with a verdict; an
        # unexpected exception reported as 1 would send someone hunting a
        # regression in code that is fine.
        print(f"::error::gate crashed ({type(exc).__name__}: {exc}) -- treated as a dead probe")
        return 2


# ---- selftest --------------------------------------------------------------
#
# Mutations are applied to the REAL sources, copied out to a temp tree. A
# synthetic fixture would be simpler than production and would stop catching the
# shapes production actually has. Coverage is per ASSERTION, not per rule.

_OFF_THREAD_REFUSAL = (
    "\t\tif (!KTP_IsGameThread())\n"
    "\t\t{\n"
    "\t\t\tg_ktp_condbg_drops.fetch_add(1, std::memory_order_relaxed);\n"
    "\t\t\treturn;\n"
    "\t\t}\n"
)
_POSIX_OPEN = '\t\tfh = FS_Open(file, "at");'
_RETRY_NOTE = (
    "\t\t// Retried per line until it succeeds, exactly as the stock path was: the\n"
    "\t\t// first Con_Printf can precede a usable filesystem, and latching a single\n"
    "\t\t// early failure would silently kill qconsole.log for the whole session.\n"
)
_POSIX_WRITE = (
    '\tif (FS_FPrintf(fh, "%s", data) < 0)\n'
    "\t\tg_ktp_condbg_drops.fetch_add(1, std::memory_order_relaxed);"
)
_SEND_SITE_NOTE = (
    '\t\t\t// Despite the "must never reach Con_Printf" rule above, this one does: a failed\n'
    "\t\t\t// sendto() logs from here (net_ws.cpp, ca_dedicated branch) into Con_DebugLog,\n"
    "\t\t\t// which is safe off-thread now. Anything else added here is not — audit it,\n"
    "\t\t\t// and CI reds on it (.github/scripts/con_thread_safety.py).\n"
)
_SEND_CALL = "\t\t\tNET_SendPacket(NS_SERVER, iLen, szOutBuf, netAdr);"

# (name, relative file, find, replace, expected exit)
_CASES: list[tuple[str, str, str, str, int]] = [
    # --- the format buffer
    ("the format buffer goes back to a shared static, in the body",
     SYS_DLL, "\tchar data[8192];", "\tstatic char data[8192];", 1),
    ("the format buffer moves to FILE SCOPE, which empties the body of statics",
     SYS_DLL, "\tchar data[8192];\n", "", 1),
    # --- the handle
    ("the handle goes back to a plain static",
     SYS_DLL, "static std::atomic<FileHandle_t> s_condbgFh(nullptr);",
     "static FileHandle_t s_condbgFh = nullptr;", 1),
    ("the release store becomes relaxed",
     SYS_DLL, "s_condbgFh.store(fh, std::memory_order_release);",
     "s_condbgFh.store(fh, std::memory_order_relaxed);", 1),
    ("the acquire load becomes relaxed",
     SYS_DLL, "FileHandle_t fh = s_condbgFh.load(std::memory_order_acquire);",
     "FileHandle_t fh = s_condbgFh.load(std::memory_order_relaxed);", 1),
    ("the handle is closed and un-published on a write failure (the use-after-free)",
     SYS_DLL, _POSIX_WRITE,
     '\tif (FS_FPrintf(fh, "%s", data) < 0)\n\t{\n'
     "\t\tg_ktp_condbg_drops.fetch_add(1, std::memory_order_relaxed);\n"
     "\t\tFS_Close(fh);\n"
     "\t\ts_condbgFh.store(nullptr, std::memory_order_release);\n\t}", 1),
    # --- the thread checks
    ("an open loses its thread check entirely",
     SYS_DLL, _OFF_THREAD_REFUSAL + _RETRY_NOTE + _POSIX_OPEN, _POSIX_OPEN, 1),
    ("a refusal keeps its test and its counter but loses the `return`",
     SYS_DLL,
     _OFF_THREAD_REFUSAL + _RETRY_NOTE + _POSIX_OPEN,
     "\t\tif (!KTP_IsGameThread())\n\t\t{\n"
     "\t\t\tg_ktp_condbg_drops.fetch_add(1, std::memory_order_relaxed);\n"
     "\t\t}\n" + _RETRY_NOTE + _POSIX_OPEN, 1),
    ("an off-thread refusal stops counting the drop",
     SYS_DLL,
     _OFF_THREAD_REFUSAL + '\t\tFileHandle_t once = FS_Open(file, "at");',
     "\t\tif (!KTP_IsGameThread())\n\t\t{\n\t\t\treturn;\n\t\t}\n"
     '\t\tFileHandle_t once = FS_Open(file, "at");', 1),
    ("the open uses FS_OpenPathID, which does not contain `FS_Open(`",
     SYS_DLL, _OFF_THREAD_REFUSAL + _RETRY_NOTE + _POSIX_OPEN,
     '\t\tfh = FS_OpenPathID(file, "at", "GAME");', 1),
    ("the write loses its failure test",
     SYS_DLL, _POSIX_WRITE, '\tFS_FPrintf(fh, "%s", data);', 1),
    # --- the path
    ("the path is resolved and opened with raw stdio",
     SYS_DLL, _POSIX_OPEN,
     '\t\tfh = (FileHandle_t)fopen(FS_GetLocalPath(file), "at");', 1),
    # --- the redirect gate
    ("the rcon-redirect capture loses its thread gate",
     SYS_DLL, "if (sv_redirected && KTP_IsGameThread())", "if (sv_redirected)", 1),
    # --- the counters
    ("g_ktp_conio_worst_us goes back to a plain scalar",
     SV_MAIN, "std::atomic<uint32> g_ktp_conio_worst_us(0);",
     "uint32 g_ktp_conio_worst_us = 0;", 1),
    ("g_ktp_conio_frame_us's definition is replaced by an extern declaration",
     SV_MAIN, "std::atomic<uint32> g_ktp_conio_frame_us(0);",
     "extern std::atomic<uint32> g_ktp_conio_frame_us;", 1),
    ("the drop counter stops being printed",
     SV_MAIN, "condbg_drops=%u", "condbg_unused=%u", 1),
    ("the io: line stops reading the drop counter",
     SV_MAIN, "g_ktp_condbg_drops.load(std::memory_order_relaxed));",
     "0u);", 1),
    # --- the Steam thread
    ("a direct Con_Printf is added to the Steam thread",
     SV_STEAM3, _SEND_CALL, '\t\t\tCon_Printf("steam send\\n");\n' + _SEND_CALL, 1),
    ("Sys_Printf is used instead, which Con_Printf itself funnels into",
     SV_STEAM3, _SEND_CALL, '\t\t\tSys_Printf("steam tick\\n");\n' + _SEND_CALL, 1),
    ("a Con_Printf is added ONE FRAME DOWN, in a background callback handler",
     SV_STEAM3, "void CSteam3Server::OnLogonFailure(SteamServerConnectFailure_t *pLogonFailure)\n{",
     "void CSteam3Server::OnLogonFailure(SteamServerConnectFailure_t *pLogonFailure)\n{\n"
     '\tCon_Printf("steam logon failed\\n");', 1),
    ("a Con_Printf is added to the refresh publisher the thread loop calls",
     SV_STEAM3, "static void SteamRefresh_ProcessIfPending()\n{",
     "static void SteamRefresh_ProcessIfPending()\n{\n"
     '\tCon_DPrintf("refresh\\n");', 1),
    ("the send site's note about the exception is deleted",
     SV_STEAM3, _SEND_SITE_NOTE, "", 1),
    # --- controls: a probe that cannot see its subject must be 2, never 0 or 1
    ("Con_DebugLog renamed away (control: dead probe, not a pass)",
     SYS_DLL, "void Con_DebugLog(const char *file", "void Con_DebugLog_x(const char *file", 2),
    ("Steam_ThreadFunc renamed away (control: dead probe, not a pass)",
     SV_STEAM3, "static void *Steam_ThreadFunc(void *)", "static void *Steam_ThreadFunc_x(void *)",
     2),
    ("a multi-line comment with an unbalanced brace inside Con_DebugLog "
     "(control: must not truncate the body)",
     SYS_DLL, _POSIX_OPEN,
     "\t\t/* the refusal shape is\n\t\t      drops++; return; }\n\t\t */\n" + _POSIX_OPEN, 0),
    ("an #if 0 block holding a Con_Printf in the Steam thread "
     "(control: dead code is not a violation)",
     SV_STEAM3, _SEND_CALL,
     '#if 0\n\t\t\tCon_Printf("old\\n");\n#endif\n' + _SEND_CALL, 0),
    ("a wrapped release store (control: a line break is not a missing memory order)",
     SYS_DLL, "s_condbgFh.store(fh, std::memory_order_release);",
     "s_condbgFh.store(fh,\n\t\t\tstd::memory_order_release);", 0),
    ("the redirect condition written the other way round "
     "(control: operand order is not the invariant)",
     SYS_DLL, "if (sv_redirected && KTP_IsGameThread())",
     "if (KTP_IsGameThread() && sv_redirected != RD_NONE)", 0),
    ("a three-line comment inside a refusal block "
     "(control: comments must not consume the drop window)",
     SYS_DLL,
     _OFF_THREAD_REFUSAL + _RETRY_NOTE + _POSIX_OPEN,
     "\t\tif (!KTP_IsGameThread())\n\t\t{\n"
     "\t\t\t// An early print can precede a usable filesystem, so this is not\n"
     "\t\t\t// an error -- it is the one case the counter exists to make visible\n"
     "\t\t\t// rather than to alarm about.\n"
     "\t\t\tg_ktp_condbg_drops.fetch_add(1, std::memory_order_relaxed);\n"
     "\t\t\treturn;\n\t\t}\n" + _RETRY_NOTE + _POSIX_OPEN, 0),
]


def selftest() -> int:
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    try:
        Gate(root)
    except GateError as exc:
        print(f"::error::selftest cannot start -- {exc}")
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        for rel in FILES:
            dst = os.path.join(tmp, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(os.path.join(root, rel.replace("/", os.sep)), dst)

        # Positive control: the unmutated copy must pass, or every "mutation
        # rejected" below proves only that the copy is broken.
        base = run(tmp)
        print(
            f"--- selftest [{'ok' if base == 0 else 'SELFTEST FAILURE'}] "
            f"unmutated sources: expected 0, got {base}\n"
        )
        failures = 0 if base == 0 else 1

        for name, rel, find, repl, expected in _CASES:
            path = os.path.join(tmp, rel.replace("/", os.sep))
            with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
                original = fh.read()
            # The mutations are written with LF; the stored sources are CRLF.
            normalized = original.replace("\r\n", "\n")
            if normalized.count(find) != 1:
                print(
                    f"--- selftest [SELFTEST FAILURE] {name}: the mutation site matched "
                    f"{normalized.count(find)} times in {rel}, not once -- the case is stale\n"
                )
                failures += 1
                continue
            try:
                with open(path, "w", encoding="utf-8", newline="") as fh:
                    fh.write(normalized.replace(find, repl))
                got = run(tmp)
            finally:
                with open(path, "w", encoding="utf-8", newline="") as fh:
                    fh.write(original)
            ok = got == expected
            if not ok:
                failures += 1
            print(
                f"--- selftest [{'ok' if ok else 'SELFTEST FAILURE'}] {name}: "
                f"expected {expected}, got {got}\n"
            )

    if failures:
        print(f"::error::{failures} selftest case(s) did not behave as declared")
        return 1
    print(f"selftest: all {len(_CASES)} cases behaved as declared and the clean tree passed")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=None, help="repo root (default: this script's repo)")
    ap.add_argument("--selftest", action="store_true",
                    help="mutate the real sources per assertion and assert each is rejected")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    root = args.root or os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    return run(root)


if __name__ == "__main__":
    sys.exit(main())
