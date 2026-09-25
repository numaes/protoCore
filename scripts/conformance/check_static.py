#!/usr/bin/env python3
"""check_static.py -- the static half of the embedder conformance suite (P4).

THE CONTRACT, and the reason it is a ratchet rather than a gate:

A static check cannot distinguish a legitimate use from a violation in every
case.  `fromUTF8String` is CORRECT for a string that is not an attribute key.
`getOwnAttributeDirect` MUST be compared against nullptr -- that is its
documented convention, and a checker that flagged it would train every reader to
ignore this tool.

So each embedder carries conformance-allow.txt: one line per known site, each
with a WRITTEN JUSTIFICATION.  The exit status is driven by the DELTA:

  * a finding not covered by the allowlist  -> exit 1 (the ratchet closed)
  * an allowlist entry whose site is gone   -> exit 2 (stale; delete the line)
  * a count that changed but is covered     -> exit 0, reported, not failed

THE RAW COUNT IS NEVER THE GATE.  A gate on the count would have to be either
zero (impossible on day one) or a magic number (meaningless on day two).

Allowlist line format, `#` comments allowed:

    <check-id> <path>:<line> <sha1-of-the-line-text> :: <justification>

The line-text hash is what makes the ratchet honest: it makes an entry stale
when the line CHANGES, not only when it moves, so an allowlisted
`fromUTF8String` that is later turned into an attribute key re-opens the
finding.  `<path>:*` is accepted as a file-level entry for the case where a
per-site justification would be a ritual rather than a review; it must carry a
justification naming the follow-up.

WHAT THIS SCRIPT DELIBERATELY DOES NOT DO.  Rule 3 in its general form --
"no ProtoObject* is held across an allocation only in a C++ local" -- is NOT
checked here.  Deciding it requires knowing, for an arbitrary call, whether it
can allocate and whether an arbitrary local is still live afterwards: escape
analysis over the whole call graph.  A checker that claimed to do it would have
a false-negative rate nobody could estimate and, being believed, would be worse
than the checklist it replaced.  Rule 3 is audited at run time by the
`gc.host_stress` case under a heap ceiling and under AddressSanitizer, and
answered by hand in each runtime's docs/CONFORMANCE.md item C3.
"""

import argparse
import hashlib
import json
import os
import re
import sys

SEVERITY_ORDER = {"error": 0, "warn": 1, "info": 2}


def sha1_of(text):
    return hashlib.sha1(text.strip().encode("utf-8", "replace")).hexdigest()[:12]


class Finding:
    def __init__(self, check, path, line, text, severity, message):
        self.check = check
        self.path = path
        self.line = line
        self.text = text
        self.severity = severity
        self.message = message

    @property
    def site(self):
        return "%s:%d" % (self.path, self.line)

    @property
    def digest(self):
        return sha1_of(self.text)

    def allow_line(self):
        return "%s %s %s :: JUSTIFY ME -- %s" % (
            self.check, self.site, self.digest, self.message)

    def __str__(self):
        return "%-5s %-24s %s:%d\n        %s\n        | %s" % (
            self.severity, self.check, self.path, self.line,
            self.message, self.text.strip()[:160])


def iter_sources(repo, subdirs):
    for sub in subdirs:
        root = os.path.join(repo, sub)
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames
                           if not d.startswith(".")
                           and not d.startswith("build")
                           and d not in ("node_modules", "_deps")]
            for fn in sorted(filenames):
                if fn.endswith((".cpp", ".cc", ".h", ".hpp")):
                    full = os.path.join(dirpath, fn)
                    rel = os.path.relpath(full, repo)
                    try:
                        with open(full, "r", encoding="utf-8",
                                  errors="replace") as fh:
                            yield rel, fh.read().split("\n")
                    except OSError:
                        continue


def strip_comment(line):
    """Good enough to keep the checks off commentary, which is most of the
    false-positive surface: several runtimes carry comments that quote the very
    patterns being looked for, as warnings against them."""
    s = line
    i = s.find("//")
    if i >= 0:
        s = s[:i]
    return s


# ---------------------------------------------------------------------------
# Rule 4 -- symbol_key_source
# ---------------------------------------------------------------------------
def check_symbol_key_source(rel, lines, rules, findings):
    factories = rules["uninterned_string_factories"]
    err_dest = set(rules["key_destinations"]["error"])
    warn_dest = set(rules["key_destinations"]["warn"])
    fac_re = re.compile(r"\b(" + "|".join(factories) + r")\s*\(")
    # A local that receives an uninterned string, so the key can be followed one
    # assignment deep.  Deeper than that is the escape analysis this script
    # declines to attempt.
    assign_re = re.compile(
        r"\b(?:const\s+)?(?:proto::)?Proto(?:String|Object)\s*\*\s*(\w+)\s*=")
    tainted = {}
    for i, raw in enumerate(lines, start=1):
        line = strip_comment(raw)
        if not line.strip():
            continue
        if fac_re.search(line):
            m = assign_re.search(line)
            if m:
                tainted[m.group(1)] = (i, raw)
        for dest in list(err_dest) + list(warn_dest):
            if dest + "(" not in line:
                continue
            # Same-line inline use: factory result straight into the name slot.
            if fac_re.search(line):
                sev = "error" if dest in err_dest else "warn"
                findings.append(Finding(
                    "symbol_key_source", rel, i, raw, sev,
                    "an uninterned string built on this line is used as the "
                    "name argument of %s()." % dest + explain_dest(dest)))
                continue
            for var, (decl_line, decl_raw) in tainted.items():
                if re.search(r"\b%s\b" % re.escape(var), line) and i - decl_line <= 12:
                    sev = "error" if dest in err_dest else "warn"
                    findings.append(Finding(
                        "symbol_key_source", rel, i, raw, sev,
                        "'%s' was built by an uninterned string factory at line "
                        "%d and reaches %s() here." % (var, decl_line, dest)
                        + explain_dest(dest)))
                    break


def explain_dest(dest):
    if dest == "getOwnAttributeDirect":
        return (
            "  THIS ONE IS SILENT: getOwnAttributeDirect has no STRING-tagged "
            "branch and no content fallback -- it probes the AttributeCache and "
            "the AVL tree on the RAW pointer.  An uninterned key therefore "
            "MISSES, and nullptr is also this function's value for 'absent', so "
            "the miss is indistinguishable from a missing attribute.  Note that "
            "names of six ASCII bytes or fewer are embedded in the pointer word "
            "(INLINE_STRING_MAX_BYTES), so short keys match by accident and long "
            "ones do not: that is why this bug presents as 'it works for k1 and "
            "not for longer_key'.  Use ProtoString::createSymbol.")
    if dest in ("setAttribute", "setAttributeIfEqual"):
        return ("  setAttribute auto-interns a STRING-tagged name, so this is "
                "CORRECT and costs one SymbolTable::intern per call -- a "
                "performance departure, not a miss.")
    return ("  This path falls back to symbolTable->lookupByContent, so it is "
            "CORRECT and costs a content hash over the rope per call -- a "
            "performance departure, not a miss.")


# ---------------------------------------------------------------------------
# Rule 6 -- attr_sentinel
# ---------------------------------------------------------------------------
def check_attr_sentinel(rel, lines, rules, findings):
    table = rules["attr_sentinel"]
    for i, raw in enumerate(lines, start=1):
        line = strip_comment(raw)
        if not line.strip():
            continue
        for fn, spec in table.items():
            if fn + "(" not in line:
                continue
            absent = spec.get("absent")
            if absent == "PROTO_FALSE":
                # Rule 6's cheapest and most silent case: the result is a
                # boolean OBJECT, so a raw boolean context is always true.
                if re.search(r"(?:if|while)\s*\(\s*!?\s*[\w\->.:()\[\]]*%s\s*\(" % re.escape(fn), line) \
                        and "PROTO_TRUE" not in line and "PROTO_FALSE" not in line \
                        and "==" not in line and "!=" not in line:
                    findings.append(Finding(
                        "attr_sentinel", rel, i, raw, "error",
                        "%s() returns a boolean OBJECT (PROTO_TRUE is 1217UL, "
                        "PROTO_FALSE is 193UL), never a C++ bool.  Both are "
                        "non-null, so this condition is ALWAYS TRUE.  Compare "
                        "against PROTO_TRUE explicitly." % fn))
                continue
            wrong = "PROTO_NONE" if absent == "nullptr" else "nullptr"
            if re.search(r"%s\s*\([^;]*\)\s*(==|!=)\s*%s\b" % (re.escape(fn), re.escape(wrong)), line):
                findings.append(Finding(
                    "attr_sentinel", rel, i, raw, "error",
                    "%s() reports 'not found' as %s, and this compares its "
                    "result against %s, which is the WRONG sentinel: the branch "
                    "can never be taken (or is taken for every object).  %s"
                    % (fn, absent, wrong, spec.get("cite", ""))))


# ---------------------------------------------------------------------------
# Rule 7 -- external_finalizer
# ---------------------------------------------------------------------------
def collect_function_bodies(lines):
    """Very rough brace matcher, adequate for 'what does this free function's
    body contain'.  It is only ever applied to a function whose name was already
    found in a fromExternalPointer call, so a miss degrades to no finding rather
    than to a wrong one."""
    bodies = {}
    fn_re = re.compile(r"^[\w:<>,\s\*&]*?\b(\w+)\s*\(\s*void\s*\*[^;]*\)\s*\{")
    i = 0
    while i < len(lines):
        m = fn_re.match(lines[i])
        if m:
            depth = lines[i].count("{") - lines[i].count("}")
            start = i
            j = i
            while depth > 0 and j + 1 < len(lines):
                j += 1
                depth += lines[j].count("{") - lines[j].count("}")
            bodies[m.group(1)] = (start + 1, lines[start:j + 1])
            i = j
        i += 1
    return bodies


def check_external_finalizer(rel, lines, rules, findings):
    bodies = collect_function_bodies(lines)
    call_re = re.compile(r"fromExternalPointer\s*\(")
    for i, raw in enumerate(lines, start=1):
        line = strip_comment(raw)
        m = call_re.search(line)
        if not m:
            continue
        # Take the argument list by balanced parentheses, not by a greedy regex:
        # a greedy `\)` swallows the caller's own closing parens and turned
        # `fromExternalPointer(this, nullptr))` into a finalizer argument of
        # "nullptr)", which silently hid every null-finalizer site in one runtime.
        # A checker that misses the thing it was written for is the defect this
        # phase exists to end, so the matcher is explicit.
        depth, start = 0, m.end() - 1
        end = None
        for k in range(start, len(line)):
            if line[k] == "(":
                depth += 1
            elif line[k] == ")":
                depth -= 1
                if depth == 0:
                    end = k
                    break
        args = line[start + 1:end] if end is not None else line[start + 1:]
        # Split on top-level commas only, so a cast or a nested call does not
        # look like a second argument.
        parts, d, cur = [], 0, ""
        for ch in args:
            if ch in "(<[":
                d += 1
            elif ch in ")>]":
                d -= 1
            if ch == "," and d == 0:
                parts.append(cur.strip()); cur = ""
            else:
                cur += ch
        if cur.strip():
            parts.append(cur.strip())
        multiline_continuation = end is None
        finalizer = parts[1] if len(parts) > 1 else ""
        if multiline_continuation and len(parts) < 2:
            # The call is split across lines; say nothing rather than guess.
            continue
        if not finalizer or finalizer == "nullptr" or finalizer == "NULL":
            findings.append(Finding(
                "external_finalizer", rel, i, raw, "error",
                "fromExternalPointer with no finalizer.  The external memory is "
                "then released by nothing protoCore knows about: legitimate only "
                "when the embedder frees it itself on a path that is guaranteed "
                "to run, which is checklist item C7 and not something this "
                "script can rule on."))
            continue
        name = finalizer.lstrip("&").split("::")[-1]
        if name not in bodies:
            continue
        start, body = bodies[name]
        text = "\n".join(strip_comment(b) for b in body)
        for pat in rules["forbidden_in_finalizer"]["block"]:
            if pat in text:
                findings.append(Finding(
                    "external_finalizer", rel, start, body[0], "error",
                    "the finalizer '%s' (used at %s:%d) BLOCKS: it contains "
                    "'%s'.  A finalizer runs on the single GC thread inside the "
                    "sweep, so a wait there stalls collection for the whole "
                    "space -- and with a heap limit configured it stalls every "
                    "mutator waiting for reclamation behind it "
                    "(docs/GarbageCollector.md section 7)."
                    % (name, rel, i, pat)))
                break
        for pat in rules["forbidden_in_finalizer"]["protocore"]:
            if pat in text:
                findings.append(Finding(
                    "external_finalizer", rel, start, body[0], "error",
                    "the finalizer '%s' (used at %s:%d) calls back into "
                    "protoCore: it contains '%s'.  The contract is absolute -- a "
                    "finalizer never allocates cells, never publishes to a "
                    "shared structure with compare-and-swap, never loops over "
                    "protoCore data and never dereferences another ProtoObject* "
                    "(docs/GarbageCollector.md section 7).  ProtoRootSet::remove "
                    "is separately forbidden because it takes the set's internal "
                    "mutex." % (name, rel, i, pat)))
                break


# ---------------------------------------------------------------------------
# Rule 12 -- critsec_across_block
# ---------------------------------------------------------------------------
def check_critsec_across_block(rel, lines, rules, findings):
    blocking = rules["blocking_calls"]
    for i, raw in enumerate(lines, start=1):
        line = strip_comment(raw)
        if "CriticalSection" not in line or "(" not in line:
            continue
        if "class CriticalSection" in line or "CriticalSection;" in line:
            continue
        # Scan forward to the end of the enclosing block.
        depth = 0
        for j in range(i - 1, min(i + 120, len(lines))):
            s = strip_comment(lines[j])
            depth += s.count("{") - s.count("}")
            if j > i - 1:
                if "UnmanagedScope" in s:
                    findings.append(Finding(
                        "critsec_across_block", rel, j + 1, lines[j], "error",
                        "an UnmanagedScope is entered while the CriticalSection "
                        "opened at line %d is still in scope.  This is doubly "
                        "wrong: the section exists to protect a half-built "
                        "structure held only in C++ locals, and the scope tells "
                        "the collector to stop waiting for this thread -- so the "
                        "root scan proceeds without those cells and the sweep "
                        "frees them." % i))
                for pat in blocking:
                    if pat in s and "//" not in lines[j].split(pat)[0][-3:]:
                        findings.append(Finding(
                            "critsec_across_block", rel, j + 1, lines[j], "error",
                            "a blocking call ('%s') is reached while the "
                            "CriticalSection opened at line %d is still in "
                            "scope.  parkForStopTheWorld skips parking while "
                            "criticalSectionDepth > 0, and safepoint() skips "
                            "young-chain submission at depth > 0, so a critical "
                            "section held across a wait reproduces rule 2's hang "
                            "AND rule 1's non-submission through a mechanism "
                            "that looks like correct code." % (pat, i)))
                        break
            if depth <= 0 and j > i - 1:
                break


# ---------------------------------------------------------------------------
# Rule 2 -- blocking_join_unbracketed (the kernel now covers ProtoThread::join;
# a direct std::thread::join on a registered thread is still the embedder's)
# ---------------------------------------------------------------------------
def check_blocking_join(rel, lines, rules, findings):
    join_re = re.compile(r"(\w+)\s*(?:\.|->)\s*join\s*\(")
    pthread_re = re.compile(r"\bpthread_join\s*\(")
    for i, raw in enumerate(lines, start=1):
        line = strip_comment(raw)
        if "join(" not in line and "pthread_join" not in line:
            continue
        m = join_re.search(line)
        if not m and pthread_re.search(line):
            window = "\n".join(strip_comment(x) for x in lines[max(0, i - 8):i])
            if "UnmanagedScope" not in window:
                findings.append(Finding(
                    "blocking_join_unbracketed", rel, i, raw, "error",
                    "pthread_join with no ProtoContext::UnmanagedScope in scope.  "
                    "protoCore cannot see this call at all, so the kernel's own "
                    "bracketing of ProtoThread::join does not help: if the "
                    "calling thread is registered it still counts in "
                    "runningThreads while it blocks and the stop-the-world "
                    "quorum can never be met."))
            continue
        if not m:
            continue
        recv = m.group(1)
        # Distinguish a THREAD join from JavaScript's Array.prototype.join,
        # which appears verbatim inside C++ raw-string polyfills in at least one
        # runtime.  A checker that reported `parts.join(',')` as a
        # stop-the-world hazard would be ignored within a day, and rightly.
        # Evidence required: a joinable() guard on this line or just above, or a
        # std::thread/jthread declaration of this receiver in the file.
        near = "\n".join(strip_comment(x) for x in lines[max(0, i - 3):i])
        looks_like_thread = (
            "joinable" in near
            or re.search(r"std::(?:thread|jthread)[\s\*&<>,:\w]*\b%s\b"
                         % re.escape(recv), "\n".join(lines))
            or re.search(r"\b%s\b\s*;?\s*//.*thread" % re.escape(recv), near))
        if not looks_like_thread:
            continue
        # protoCore's own API: it brackets itself as of 2026-09-25.
        is_proto_join = bool(re.search(r"join\s*\(\s*&?\w*(ctx|context|Ctx|Context)", line))
        # Look back a few lines for a guard.
        window = "\n".join(strip_comment(x) for x in lines[max(0, i - 8):i])
        guarded = "UnmanagedScope" in window
        if is_proto_join:
            if guarded:
                findings.append(Finding(
                    "blocking_join_unbracketed", rel, i, raw, "info",
                    "ProtoThread::join with an UnmanagedScope guard.  Harmless "
                    "and idempotent -- unmanagedDepth is a counter and only the "
                    "outermost pair moves parkedThreads -- but redundant since "
                    "2026-09-25: the kernel brackets this call itself.  Keep the "
                    "guard or drop it; the comment beside it may now be stale."))
            continue
        if not guarded:
            findings.append(Finding(
                "blocking_join_unbracketed", rel, i, raw, "error",
                "a direct join on '%s' with no ProtoContext::UnmanagedScope in "
                "scope.  protoCore's own ProtoThread::join leaves the running "
                "set for you; a raw std::thread::join or pthread_join does NOT. "
                " If the calling thread is registered with protoCore -- and the "
                "main thread is, from ProtoSpace construction -- it still counts "
                "in runningThreads while it blocks, the stop-the-world quorum "
                "can never be met, and a thread that then needs memory waits for "
                "a cycle that cannot begin.  That is a deadlock, not slow "
                "shutdown." % recv))


# ---------------------------------------------------------------------------
# Informational: rule 8's and rule 11's preconditions
# ---------------------------------------------------------------------------
def check_heap_policy(repo, subdirs, findings):
    seen = {"setHeapLimits": False, "outOfMemoryCallback": False}
    for rel, lines in iter_sources(repo, subdirs):
        for raw in lines:
            line = strip_comment(raw)
            for k in seen:
                if k in line:
                    seen[k] = True
    if not seen["setHeapLimits"]:
        findings.append(Finding(
            "heap_policy", "<repository>", 0, "", "info",
            "this runtime never calls ProtoSpace::setHeapLimits, so maxHeapSize "
            "is 0, the ceiling is disabled and waitForHeapHeadroom returns "
            "immediately.  Two consequences: rule 8 is UNREACHABLE in this "
            "runtime as configured -- the finding is latent, not fixed -- and "
            "no cycle ever starts by itself, because a heap limit is what makes "
            "a thread need cells the heap cannot supply.  Whether a runtime "
            "should bound its heap is a maintainer decision, so this is "
            "reported and not failed."))
    if not seen["outOfMemoryCallback"]:
        findings.append(Finding(
            "heap_policy", "<repository>", 0, "", "info",
            "this runtime installs no outOfMemoryCallback, which is the one "
            "recovery hook before the controlled abort in waitForHeapHeadroom."))


CHECKS = {
    "symbol_key_source": check_symbol_key_source,
    "attr_sentinel": check_attr_sentinel,
    "external_finalizer": check_external_finalizer,
    "critsec_across_block": check_critsec_across_block,
    "blocking_join_unbracketed": check_blocking_join,
}


def load_allowlist(path):
    entries = {}
    if not path or not os.path.isfile(path):
        return entries
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if "::" not in line:
                continue
            head, justification = line.split("::", 1)
            parts = head.split()
            if len(parts) < 2:
                continue
            check, site = parts[0], parts[1]
            digest = parts[2] if len(parts) > 2 else "*"
            entries[(check, site)] = (digest, justification.strip())
    return entries


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Exit: 0 clean (findings all covered) | 1 an uncovered finding "
               "(the ratchet closed) | 2 a stale allowlist entry | 3 usage.")
    ap.add_argument("--repo", help="the embedder repository root")
    ap.add_argument("--src", action="append", default=None,
                    help="source subdirectory to scan (repeatable; default src)")
    ap.add_argument("--allow", default=None,
                    help="allowlist file (default <repo>/conformance-allow.txt)")
    ap.add_argument("--check", action="append", default=None,
                    help="run only this check (repeatable)")
    ap.add_argument("--init-allowlist", action="store_true",
                    help="print an allowlist covering every current finding, "
                         "with JUSTIFY ME placeholders a human must replace")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true",
                    help="run the built-in fixture suite and exit")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "rules.json"), "r", encoding="utf-8") as fh:
        rules = json.load(fh)

    if args.self_test:
        return self_test(rules)
    if not args.repo:
        ap.error("--repo is required unless --self-test is given")

    subdirs = args.src or ["src"]
    allow_path = args.allow or os.path.join(args.repo, "conformance-allow.txt")
    allow = load_allowlist(allow_path)

    wanted = set(args.check) if args.check else set(CHECKS)
    findings = []
    for rel, lines in iter_sources(args.repo, subdirs):
        for name, fn in CHECKS.items():
            if name in wanted:
                fn(rel, lines, rules, findings)
    if not args.check or "heap_policy" in wanted or not args.check:
        check_heap_policy(args.repo, subdirs, findings)

    if args.init_allowlist:
        print("# conformance-allow.txt -- the static ratchet for %s"
              % os.path.basename(os.path.normpath(args.repo)))
        print("# Format: <check-id> <path>:<line> <sha1-12> :: <justification>")
        print("# Every JUSTIFY ME below must be replaced by a human before this")
        print("# file means anything.  An entry whose line CHANGES goes stale and")
        print("# re-opens the finding, which is the point.")
        for f in sorted(findings, key=lambda f: (SEVERITY_ORDER[f.severity],
                                                 f.path, f.line)):
            if f.severity == "info":
                continue
            print(f.allow_line())
        return 0

    covered, uncovered = [], []
    for f in findings:
        if f.severity == "info":
            continue
        key = (f.check, f.site)
        file_key = (f.check, f.path + ":*")
        entry = allow.get(key) or allow.get(file_key)
        if entry is None:
            uncovered.append(f)
            continue
        digest, _ = entry
        if digest != "*" and key in allow and digest != f.digest:
            f.message = ("this site is allowlisted, but the LINE HAS CHANGED "
                         "since it was justified (allowlist %s, now %s), so the "
                         "justification no longer applies: re-read it.  "
                         % (digest, f.digest)) + f.message
            uncovered.append(f)
            continue
        covered.append(f)

    present_sites = {(f.check, f.site) for f in findings}
    stale = [k for k in allow
             if not k[1].endswith(":*") and k not in present_sites]

    if args.json:
        print(json.dumps({
            "repo": args.repo,
            "counts": {
                "uncovered": len(uncovered),
                "covered": len(covered),
                "stale_allowlist_entries": len(stale),
                "info": sum(1 for f in findings if f.severity == "info"),
            },
            "uncovered": [{"check": f.check, "site": f.site,
                           "severity": f.severity, "message": f.message}
                          for f in uncovered],
            "stale": ["%s %s" % k for k in sorted(stale)],
            "info": [f.message for f in findings if f.severity == "info"],
            # Stated in the output, not only in the docstring: a consumer of this
            # JSON must not read a clean result as "rule 3 holds".
            "not_checked_here": [
                "rule 3 in general (no ProtoObject* across an allocation in a "
                "bare C++ local) -- whole-program escape analysis; audited at "
                "run time by gc.host_stress under ASan and answered by hand in "
                "docs/CONFORMANCE.md C3",
                "whether a null finalizer is correct (C7)",
                "whether an external byte total is accurate (C7)",
            ],
        }, indent=2))
    else:
        print("=== conformance static check: %s ===" % args.repo)
        for f in sorted(uncovered, key=lambda f: (SEVERITY_ORDER[f.severity],
                                                  f.path, f.line)):
            print(f)
        print("\n--- informational ---")
        for f in findings:
            if f.severity == "info":
                print("info  %s\n        %s" % (f.check, f.message))
        print("\ncovered by allowlist: %d   uncovered: %d   stale entries: %d"
              % (len(covered), len(uncovered), len(stale)))
        for k in sorted(stale):
            print("stale: %s %s -- the site no longer exists; delete the line"
                  % k)
        print("\nNOT CHECKED HERE: rule 3 in general is whole-program escape "
              "analysis and is deliberately not attempted; it is audited at run "
              "time by gc.host_stress under AddressSanitizer and answered by "
              "hand in docs/CONFORMANCE.md item C3.")

    if stale:
        return 2
    if uncovered:
        return 1
    return 0


# ---------------------------------------------------------------------------
# The script's own tests.  A checker nobody has shown to fire is in exactly the
# position this whole phase exists to end.
# ---------------------------------------------------------------------------
FIXTURES = [
    ("attr_sentinel", "error", """
void f(proto::ProtoContext* ctx, const proto::ProtoObject* o,
       const proto::ProtoString* k) {
    if (o->getAttribute(ctx, k) != nullptr) { use(); }
}
"""),
    ("attr_sentinel", "error", """
void g(proto::ProtoContext* ctx, const proto::ProtoObject* o,
       const proto::ProtoString* k) {
    if (o->hasAttribute(ctx, k)) { use(); }
}
"""),
    ("symbol_key_source", "error", """
void h(proto::ProtoContext* ctx, const proto::ProtoObject* o) {
    const proto::ProtoString* key = ctx->fromUTF8String("a_long_attribute_name")->asString(ctx);
    o->getOwnAttributeDirect(ctx, key);
}
"""),
    ("external_finalizer", "error", """
void freeIt(void* p) {
    State* s = (State*) p;
    if (s->thread.joinable()) s->thread.join();
    delete s;
}
void reg(proto::ProtoContext* ctx, State* s) {
    ctx->fromExternalPointer(s, freeIt);
}
"""),
    ("external_finalizer", "error", """
void freeIt2(void* p) {
    State* s = (State*) p;
    rs->remove(s->pin);
    delete s;
}
void reg2(proto::ProtoContext* ctx, State* s) {
    ctx->fromExternalPointer(s, freeIt2);
}
"""),
    ("critsec_across_block", "error", """
void k(proto::ProtoContext* ctx) {
    proto::ProtoContext::CriticalSection cs(ctx);
    {
        proto::ProtoContext::UnmanagedScope u(ctx);
        doIo();
    }
}
"""),
    ("blocking_join_unbracketed", "error", """
void shutdown(std::vector<std::thread>& ts) {
    for (auto& t : ts) if (t.joinable()) t.join();
}
"""),
]

NEGATIVES = [
    ("attr_sentinel", """
void ok(proto::ProtoContext* ctx, const proto::ProtoObject* o,
        const proto::ProtoString* k) {
    if (o->getOwnAttributeDirect(ctx, k) == nullptr) { absent(); }
    if (o->getAttribute(ctx, k) == PROTO_NONE) { absent(); }
    if (o->hasAttribute(ctx, k) == PROTO_TRUE) { present(); }
}
"""),
    ("symbol_key_source", """
void ok2(proto::ProtoContext* ctx, const proto::ProtoObject* o) {
    const proto::ProtoString* key = proto::ProtoString::createSymbol(ctx, "a_long_attribute_name");
    o->getOwnAttributeDirect(ctx, key);
    const proto::ProtoString* label = ctx->fromUTF8String("not a key")->asString(ctx);
    print(label);
}
"""),
    # JavaScript's Array.prototype.join, embedded verbatim in a C++ raw-string
    # polyfill.  A checker that reported this as a stop-the-world hazard would be
    # switched off within a day, and rightly.
    ("blocking_join_unbracketed", """
static const char* kPolyfill = R"JS(
    var parts = [];
    return '[' + parts.join(',') + ']';
)JS";
"""),
    ("blocking_join_unbracketed", """
void ok3(proto::ProtoContext* ctx, proto::ProtoThread* t,
         std::thread& raw) {
    {
        proto::ProtoContext::UnmanagedScope u(ctx);
        raw.join();
    }
}
"""),
]


def self_test(rules):
    ok = True
    for check, severity, src in FIXTURES:
        findings = []
        CHECKS[check]("fixture.cpp", src.split("\n"), rules, findings)
        hits = [f for f in findings if f.check == check and f.severity == severity]
        if not hits:
            ok = False
            print("SELF-TEST FAIL: %s did not fire at severity %s on its "
                  "positive fixture -- the check cannot detect the bug it "
                  "exists for.\n%s" % (check, severity, src))
        else:
            print("ok   %-26s fires on its positive fixture (%d finding(s))"
                  % (check, len(hits)))
    for check, src in NEGATIVES:
        findings = []
        CHECKS[check]("fixture.cpp", src.split("\n"), rules, findings)
        hits = [f for f in findings
                if f.check == check and f.severity in ("error", "warn")]
        if hits:
            ok = False
            print("SELF-TEST FAIL: %s fired on CORRECT code, which would train "
                  "every reader to ignore it:\n  %s" % (check, hits[0]))
        else:
            print("ok   %-26s stays quiet on correct code" % check)
    print("\n%s" % ("self-test PASSED" if ok else "self-test FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
