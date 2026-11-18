#!/usr/bin/env python3
"""Repository hygiene checks for Dzjs Trainer.

Runs offline, needs only Python 3.8+. Used by .github/workflows/ci.yml and by
maintainers before a release:

    python tools/verify_repo.py           # warnings only
    python tools/verify_repo.py --strict  # non-zero exit on any finding

Checks:
  1. no unresolved merge-conflict markers in tracked text files
  2. every resource path (.rc ICON/BIN, .vcxproj Image/None/Embedded*) exists
     (build outputs under Release/, x64/Release/ and JiYuAvRelease/ excepted)
  3. every #include "..." in first-party sources resolves
  4. no tracked build artifacts (Release/, obj/, bin/, *.sys, *.pdb, ...)
  5. every project referenced by a .sln exists
  6. the repository root contains only known entries

Exit codes: 0 clean, 1 findings (strict), 2 usage or environment error.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

TEXT_SUFFIXES = {
    ".c", ".h", ".cpp", ".hpp", ".rc", ".vcxproj", ".filters", ".sln", ".md",
    ".ps1", ".py", ".cmd", ".sh", ".php", ".js", ".mjs", ".html", ".css",
    ".yml", ".yaml", ".json", ".example", ".txt", ".def", ".inf", ".gitignore",
    ".gitattributes", ".editorconfig",
}

CONFLICT_MARKER = re.compile(rb"^(<{7}|={7}|>{7})(?:\s|$)", re.MULTILINE)

# Paths that only exist after a build; their absence in a clean clone is expected.
BUILD_PATH_PREFIXES = (
    "..\\Release\\",
    "..\\Debug\\",
    "..\\x64\\Release\\",
    "..\\x64\\Debug\\",
    "..\\JiYuAvRelease\\",
    "$(SolutionDir)Release",
    "$(SolutionDir)Debug",
    "$(SolutionDir)x64",
    "$(SolutionDir)JiYuAvRelease",
    "$(SolutionDir)$(Configuration)",
    "$(SolutionDir)obj",
    "$(ProjectDir)Release",
    "$(ProjectDir)obj",
)

# Includes that deliberately point outside the repository.
EXTERNAL_INCLUDES = {
    "../../DllInjector/DllInjector.h",
}

# Project items that live outside the repository (same documented gap as above).
EXTERNAL_PROJECT_ITEMS = (
    "..\\..\\DllInjector\\",
    "../../DllInjector/",
)

ARTIFACT_SUFFIXES = {
    ".sys", ".pdb", ".obj", ".lib", ".exp", ".ilk", ".idb", ".iobj", ".ipdb",
    ".tlog", ".lastbuildstate", ".recipe", ".aps", ".res", ".user",
}

ROOT_ALLOWLIST = {
    ".editorconfig", ".gitattributes", ".gitignore", ".github",
    "CHANGELOG.md", "CONTRIBUTING.md", "LICENSE", "README.md",
    "DzjsTrainer.sln", "JiYuAv.sln",
    "assets", "docs", "include", "resources", "scripts", "third_party",
    "DzjsTrainer", "DzjsTrainerUI", "DzjsTrainerDriver", "DzjsTrainerHooks",
    "DzjsTrainerUpdater", "JiYuAvKernel", "JiYuAvCtl", "JiYuAvShared",
    "JiYuAvPatternTests", "CaptureProbe", "GuardTerminator", "PhoneDialogProbe",
    "ExitInputSimulator", "DzjsTrainerInjector64", "DriverPublisherTests",
    "tools", "update-server", "website",
}

RC_PATH = re.compile(r'^\s*[A-Za-z0-9_]+\s+(?:ICON|BIN)\s+"([^"]+)"', re.MULTILINE)
PROJ_PATH = re.compile(
    r'<(?:Image|None|ClCompile|ClInclude|ResourceCompile|Inf|Embedded[A-Za-z]*Resource)'
    r'\s+Include="([^"]+)"',
    re.IGNORECASE,
)
SLN_PROJECT = re.compile(r'^Project\("[^"]*"\)\s*=\s*"([^"]+)",\s*"([^"]+)"', re.MULTILINE)
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def tracked_files() -> list[Path]:
    try:
        out = subprocess.run(
            ["git", "ls-files", "-z"],
            cwd=ROOT,
            check=True,
            capture_output=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:  # pragma: no cover
        raise SystemExit(f"error: not a git checkout? {exc}")
    return [ROOT / p.decode("utf-8", "surrogateescape") for p in out.split(b"\0") if p]


def rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def decode(path: Path) -> str | None:
    data = path.read_bytes()
    if data.startswith(b"\xff\xfe"):
        return data[2:].decode("utf-16-le", "replace")
    if data.startswith(b"\xef\xbb\xbf"):
        return data[3:].decode("utf-8", "replace")
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode("gbk", "replace")


def is_build_path(value: str) -> bool:
    v = re.sub(r"\\{2,}", r"\\", value.strip().replace("/", "\\"))
    return any(v.startswith(p.replace("/", "\\")) for p in BUILD_PATH_PREFIXES)


def resolve_reference(base: Path, value: str) -> Path | None:
    """Resolve an MSBuild/resource path relative to `base`.

    Expands the variables that are knowable offline ($(SolutionDir),
    $(ProjectDir), $(MSBuildProjectDirectory)); anything else is unknown.
    """
    expanded = (
        value.replace("$(SolutionDir)", str(ROOT) + os.sep)
        .replace("$(MSBuildProjectDirectory)", str(base))
        .replace("$(ProjectDir)", str(base) + os.sep)
    )
    if "$(" in expanded:
        return None
    path = Path(expanded.replace("\\", "/"))
    if not path.is_absolute():
        path = base / path
    return path.resolve()


def check_conflicts(files, findings) -> None:
    for path in files:
        if path.suffix.lower() not in TEXT_SUFFIXES and path.name not in {
            ".gitignore", ".gitattributes", ".editorconfig"
        }:
            continue
        if rel(path).startswith("third_party/curl/"):
            continue
        data = path.read_bytes()
        for m in CONFLICT_MARKER.finditer(data):
            line = data[: m.start()].count(b"\n") + 1
            findings.append(f"conflict marker: {rel(path)}:{line}")


def check_resources(files, findings, warnings) -> None:
    for path in files:
        suffix = path.suffix.lower()
        if suffix not in {".rc", ".vcxproj", ".filters"}:
            continue
        text = decode(path)
        if text is None:
            continue
        patterns = [RC_PATH] if suffix == ".rc" else [PROJ_PATH]
        for pattern in patterns:
            for value in pattern.findall(text):
                if is_build_path(value):
                    continue
                if any(value.replace("/", "\\").startswith(p.replace("/", "\\"))
                       for p in EXTERNAL_PROJECT_ITEMS):
                    warnings.append(f"external project item (documented gap): {rel(path)} -> {value}")
                    continue
                target = resolve_reference(path.parent, value)
                if target is None:
                    warnings.append(f"unresolved MSBuild variable: {rel(path)} -> {value}")
                    continue
                if not target.exists():
                    findings.append(f"missing resource: {rel(path)} -> {value}")


def check_includes(files, findings, warnings) -> None:
    include_roots = [ROOT / "include", ROOT / "third_party" / "curl", ROOT / "third_party" / "curl" / "curl"]
    for path in files:
        if path.suffix.lower() not in {".c", ".h", ".cpp", ".hpp"}:
            continue
        r = rel(path)
        if r.startswith("third_party/") or "/legacy-x86/" in r:
            continue
        text = decode(path)
        if text is None:
            continue
        for value in INCLUDE.findall(text):
            norm = value.replace("\\", "/")
            if norm in EXTERNAL_INCLUDES:
                warnings.append(f"external include (documented gap): {r} -> {value}")
                continue
            candidates = [(path.parent / norm).resolve()]
            candidates += [(root / norm).resolve() for root in include_roots]
            if any(c.exists() for c in candidates):
                continue
            # Build-time generated headers fall back to a checked-in .example
            # (e.g. UpdaterTicketKey.h -> UpdaterTicketKey.h.example).
            if any(Path(str(c) + ".example").exists() for c in candidates):
                warnings.append(f"generated include uses .example fallback: {r} -> {value}")
                continue
            findings.append(f"missing include: {r} -> {value}")


def check_artifacts(files, findings) -> None:
    for path in files:
        r = rel(path)
        if any(part in {"Release", "obj", "bin", "x64", "Debug"} for part in path.relative_to(ROOT).parts[:-1]):
            findings.append(f"build directory tracked: {r}")
        if path.suffix.lower() in ARTIFACT_SUFFIXES:
            findings.append(f"build artifact tracked: {r}")


def check_solutions(files, findings) -> None:
    for path in files:
        if path.suffix.lower() != ".sln":
            continue
        text = decode(path) or ""
        for _name, project in SLN_PROJECT.findall(text):
            target = resolve_reference(path.parent, project)
            if target is not None and not target.exists():
                findings.append(f"missing solution project: {rel(path)} -> {project}")


def is_ignored(name: str) -> bool:
    """True when .gitignore (or .git/info/exclude) hides this root entry.

    Build outputs such as dist/ are intentionally absent from the tree, so they
    must not be reported as layout violations.
    """
    try:
        result = subprocess.run(
            ["git", "check-ignore", "--quiet", "--no-index", name],
            cwd=ROOT,
            capture_output=True,
        )
    except OSError:  # pragma: no cover - git missing
        return False
    return result.returncode == 0


def check_root(findings, warnings) -> None:
    for entry in sorted(ROOT.iterdir()):
        if entry.name.startswith(".tmp-"):
            warnings.append(f"transient file in root: {entry.name}")
            continue
        if entry.name == ".git":
            continue
        if entry.name not in ROOT_ALLOWLIST and not is_ignored(entry.name):
            findings.append(f"unexpected root entry: {entry.name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--strict", action="store_true", help="exit 1 on findings")
    args = parser.parse_args()

    files = tracked_files()
    findings: list[str] = []
    warnings: list[str] = []

    check_conflicts(files, findings)
    check_resources(files, findings, warnings)
    check_includes(files, findings, warnings)
    check_artifacts(files, findings)
    check_solutions(files, findings)
    check_root(findings, warnings)

    for w in warnings:
        print(f"warn: {w}")
    for f in findings:
        print(f"FAIL: {f}")

    print(
        f"\n{len(files)} tracked files, {len(findings)} finding(s), {len(warnings)} warning(s)"
    )
    if findings and args.strict:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
