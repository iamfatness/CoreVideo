#!/usr/bin/env python3
"""Summarize non-sensitive Cppcheck findings for public GitHub issues.

Security-sensitive scanner output should stay in GitHub code scanning,
workflow logs, or private advisories. This script intentionally reports only
style, performance, portability, and information severities.
"""

from __future__ import annotations

import argparse
import html
import sys
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path


PUBLIC_SEVERITIES = {"style", "performance", "portability", "information"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("xml", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=40)
    args = parser.parse_args()

    if not args.xml.exists():
        args.out.write_text("No Cppcheck XML report was produced.\n", encoding="utf-8")
        return 0

    root = ET.parse(args.xml).getroot()
    findings: list[dict[str, str]] = []
    for error in root.findall(".//error"):
        severity = error.attrib.get("severity", "")
        if severity not in PUBLIC_SEVERITIES:
            continue

        location = error.find("location")
        file_name = location.attrib.get("file", "") if location is not None else ""
        line = location.attrib.get("line", "") if location is not None else ""
        findings.append(
            {
                "severity": severity,
                "id": error.attrib.get("id", ""),
                "message": html.unescape(error.attrib.get("msg", "")),
                "file": file_name,
                "line": line,
            }
        )

    if not findings:
        args.out.write_text(
            "No public code-quality findings were reported by Cppcheck.\n",
            encoding="utf-8",
        )
        return 0

    counts = Counter(item["severity"] for item in findings)
    lines = [
        "<!-- corevideo-cppcheck-quality -->",
        "# CoreVideo Code Quality Findings",
        "",
        "Cppcheck reported non-sensitive maintainability findings. Security-sensitive scanner output is intentionally not mirrored into public issues.",
        "",
        "## Summary",
        "",
    ]
    for severity in sorted(counts):
        lines.append(f"- `{severity}`: {counts[severity]}")

    lines.extend(["", "## Top Findings", ""])
    for item in findings[: args.limit]:
        where = item["file"]
        if item["line"]:
            where = f"{where}:{item['line']}"
        lines.append(
            f"- `{item['severity']}` `{item['id']}` {where} - {item['message']}"
        )

    remaining = len(findings) - args.limit
    if remaining > 0:
        lines.extend(["", f"_Plus {remaining} additional findings in the workflow artifact._"])

    args.out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
