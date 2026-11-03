#!/usr/bin/env python3
"""Scan Spatial Echo audio hot paths for realtime-unsafe constructs."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


SEVERITY_RANK = {"none": 99, "high": 3, "medium": 2, "low": 1}


@dataclass(frozen=True)
class Pattern:
    category: str
    severity: str
    regex: re.Pattern[str]
    rationale: str


@dataclass
class FunctionScope:
    file: str
    name: str
    start_line: int
    end_line: int
    lines: list[tuple[int, str]]


@dataclass
class Finding:
    severity: str
    category: str
    file: str
    line: int
    function: str
    text: str
    rationale: str


HOT_FUNCTION_RE = re.compile(
    r"\b("
    r"updateParameters|"
    r"processBlock|processBlockBypassed|ProcessBuffer|ProcessSample|"
    r"processSample|processSamples|processRecordChainOnly|processPlaybackOnly|"
    r"process|render|tick"
    r")\b"
)

CONTROL_RE = re.compile(
    r"^\s*(if|for|while|switch|catch|else|do|try|class|struct|namespace|enum)\b"
)

PATTERNS: tuple[Pattern, ...] = (
    Pattern(
        "dynamic-allocation",
        "high",
        re.compile(
            r"\b(new|delete)\b|::operator\s+new|std::make_(unique|shared)\s*<|"
            r"\b(malloc|calloc|realloc|free)\s*\("
        ),
        "Dynamic allocation can block or take unbounded time on the audio thread.",
    ),
    Pattern(
        "locking-or-thread-sync",
        "high",
        re.compile(
            r"\b(std::lock_guard|std::unique_lock|std::scoped_lock|std::mutex|"
            r"std::condition_variable|juce::ScopedLock|ScopedLock|CriticalSection|"
            r"MessageManagerLock)\b"
        ),
        "Locks and message-thread synchronization can stall realtime audio processing.",
    ),
    Pattern(
        "blocking-wait",
        "high",
        re.compile(
            r"\b(sleep_for|sleep_until|wait_for|wait_until|join|std::async)\s*\("
        ),
        "Blocking waits can underrun or deadlock the host audio callback.",
    ),
    Pattern(
        "file-ui-or-logging",
        "medium",
        re.compile(
            r"\b(DBG|Logger::writeToLog|std::cout|std::cerr|printf|fprintf|"
            r"FileOutputStream|FileInputStream|std::ofstream|std::ifstream)\b"
        ),
        "I/O and logging can allocate, block, or call into non-realtime subsystems.",
    ),
    Pattern(
        "wall-clock-time",
        "medium",
        re.compile(
            r"\b(?:juce::)?Time::get(?:CurrentTime|MillisecondCounter(?:HiRes)?|"
            r"HighResolutionTicks(?:PerSecond)?)\s*\(|"
            r"\bstd::chrono::[A-Za-z_][A-Za-z0-9_:]*::now\s*\("
        ),
        "Wall-clock and high-resolution timer APIs can call into non-realtime subsystems or add jitter.",
    ),
    Pattern(
        "exception-path",
        "medium",
        re.compile(r"\b(throw|try|catch)\b"),
        "Exception paths are not deterministic enough for audio-thread code.",
    ),
    Pattern(
        "container-growth",
        "medium",
        re.compile(r"\.(resize|reserve|push_back|emplace_back|insert|erase)\s*\("),
        "Container growth or mutation can allocate or perform unbounded work.",
    ),
    Pattern(
        "string-construction",
        "low",
        re.compile(r"\b(std::string|juce::String|StringArray)\b"),
        "String construction and formatting often allocate in hot paths.",
    ),
)


def strip_line_comment(line: str) -> str:
    if "RTSAFETY_ALLOW" in line or "NOLINT(rt-safety)" in line:
        return ""
    if line.lstrip().startswith(("*", "/*", "*/")):
        return ""
    return line.split("//", 1)[0]


def iter_source_files(root: Path, include_dependencies: bool) -> Iterable[Path]:
    roots = [
        root / "plugin" / "Source" / "PluginProcessor.cpp",
        root / "plugin" / "Source" / "PluginProcessor.h",
        root / "plugin" / "Source" / "PluginProcessorDebug.cpp",
        root / "plugin" / "Source" / "DSP",
    ]
    suffixes = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}

    seen: set[Path] = set()
    for item in roots:
        if not item.exists():
            continue
        candidates = [item] if item.is_file() else item.rglob("*")
        for path in candidates:
            if not path.is_file() or path.suffix.lower() not in suffixes:
                continue
            if not include_dependencies and "dependencies" in path.parts:
                continue
            resolved = path.resolve()
            if resolved not in seen:
                seen.add(resolved)
                yield path


def function_name(signature: str) -> str | None:
    match = HOT_FUNCTION_RE.search(signature)
    if not match:
        return None
    return match.group(1)


def strip_signature_comments(signature: str) -> str:
    return "\n".join(line.split("//", 1)[0] for line in signature.splitlines())


def looks_like_function_signature(signature: str) -> bool:
    collapsed = " ".join(strip_signature_comments(signature).strip().split())
    if not collapsed or CONTROL_RE.match(collapsed):
        return False
    if "(" not in collapsed or ")" not in collapsed:
        return False
    if collapsed.endswith(";"):
        return False
    return function_name(collapsed) is not None


def find_function_signature_lines(pending: list[tuple[int, str]]) -> list[tuple[int, str]] | None:
    for start_index in range(len(pending) - 1, -1, -1):
        candidate = pending[start_index:]
        signature = "\n".join(item[1] for item in candidate)
        if looks_like_function_signature(signature):
            return candidate
    return None


def brace_delta(text: str) -> int:
    return text.count("{") - text.count("}")


def extract_hot_scopes(path: Path, root: Path) -> list[FunctionScope]:
    try:
        raw_lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
    except OSError:
        return []

    scopes: list[FunctionScope] = []
    pending: list[tuple[int, str]] = []
    index = 0
    while index < len(raw_lines):
        line_no = index + 1
        line = raw_lines[index]
        pending.append((line_no, line))

        if "{" not in line:
            index += 1
            if ";" in line:
                pending = []
                continue
            if len(pending) > 12:
                pending = pending[-6:]
            continue

        signature_lines = find_function_signature_lines(pending)
        if signature_lines is None:
            pending = []
            index += 1
            continue

        signature = "\n".join(item[1] for item in signature_lines)
        name = function_name(signature) or "<unknown>"
        start_line = signature_lines[0][0]
        scope_lines = signature_lines[:]
        depth = sum(brace_delta(item[1]) for item in signature_lines)
        index += 1
        while index < len(raw_lines) and depth > 0:
            next_line_no = index + 1
            next_line = raw_lines[index]
            scope_lines.append((next_line_no, next_line))
            depth += brace_delta(next_line)
            index += 1

        scopes.append(
            FunctionScope(
                file=str(path.relative_to(root)),
                name=name,
                start_line=start_line,
                end_line=scope_lines[-1][0],
                lines=scope_lines,
            )
        )
        pending = []

    return scopes


def scan_scope(scope: FunctionScope) -> list[Finding]:
    findings: list[Finding] = []
    for line_no, raw_line in scope.lines:
        searchable = strip_line_comment(raw_line)
        if not searchable.strip():
            continue
        for pattern in PATTERNS:
            if pattern.regex.search(searchable):
                findings.append(
                    Finding(
                        severity=pattern.severity,
                        category=pattern.category,
                        file=scope.file,
                        line=line_no,
                        function=scope.name,
                        text=raw_line.strip(),
                        rationale=pattern.rationale,
                    )
                )
    return findings


def should_fail(findings: list[Finding], fail_on: str) -> bool:
    if fail_on == "none":
        return False
    threshold = SEVERITY_RANK[fail_on]
    return any(SEVERITY_RANK[finding.severity] >= threshold for finding in findings)


def print_text_report(scopes: list[FunctionScope], findings: list[Finding]) -> None:
    counts = {"high": 0, "medium": 0, "low": 0}
    for finding in findings:
        counts[finding.severity] += 1

    print(
        "RT-safety audit: "
        f"scanned {len(scopes)} hot functions; "
        f"findings high={counts['high']} medium={counts['medium']} low={counts['low']}"
    )

    if not findings:
        print("No suspicious realtime-safety constructs found in scanned hot paths.")
        return

    for finding in sorted(
        findings,
        key=lambda item: (-SEVERITY_RANK[item.severity], item.file, item.line),
    ):
        print(
            f"[{finding.severity}] {finding.category} "
            f"{finding.file}:{finding.line} in {finding.function}"
        )
        print(f"  {finding.text}")
        print(f"  {finding.rationale}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Scan Spatial Echo audio hot paths for realtime-unsafe constructs."
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path.cwd(),
        help="Repository root. Defaults to the current working directory.",
    )
    parser.add_argument(
        "--include-dependencies",
        action="store_true",
        help="Also scan plugin/Source/DSP/dependencies.",
    )
    parser.add_argument(
        "--fail-on",
        choices=("none", "high", "medium", "low"),
        default="high",
        help="Exit non-zero when findings at this severity or higher are present.",
    )
    parser.add_argument(
        "--format",
        choices=("text", "json"),
        default="text",
        help="Report format.",
    )
    args = parser.parse_args(argv)

    root = args.root.resolve()
    scopes: list[FunctionScope] = []
    for path in iter_source_files(root, include_dependencies=args.include_dependencies):
        scopes.extend(extract_hot_scopes(path, root))

    findings: list[Finding] = []
    for scope in scopes:
        findings.extend(scan_scope(scope))

    if args.format == "json":
        payload = {
            "scanned_function_count": len(scopes),
            "findings": [asdict(finding) for finding in findings],
        }
        print(json.dumps(payload, indent=2))
    else:
        print_text_report(scopes, findings)

    return 1 if should_fail(findings, args.fail_on) else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
