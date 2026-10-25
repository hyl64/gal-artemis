#!/usr/bin/env python3
# Fill FINDINGS.md's coverage section from the per-parser summaries run.sh wrote.
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
FINDINGS = HERE / "FINDINGS.md"

ROWS = []
MUTATIONS = None
for path in sorted((HERE / "crashes").glob("*.summary")):
    fields = {}
    for part in path.read_text().strip().split(" "):
        if "=" in part:
            key, value = part.split("=", 1)
            fields[key] = value
    ROWS.append(fields)

if not ROWS:
    sys.exit("no summaries found - run ./run.sh first")

ORDER = ["pfs", "ast", "iet", "ipt", "asb"]
ROWS.sort(key=lambda f: ORDER.index(f["parser"]) if f["parser"] in ORDER else 99)

header = ("| 解析器 | 种子 | 输入 | ok | 明确报错 | crash | timeout | oom | "
          "silent-reject | error-success | bad-exception | 最慢 | 峰值内存 |")
rule = "|---|---|---|---|---|---|---|---|---|---|---|---|---|"
lines = [header, rule]
totals = {key: 0 for key in ("inputs", "ok", "rejected", "crash", "timeout", "oom",
                             "silent", "error_success", "bad_exception")}
for fields in ROWS:
    lines.append("| `{}` | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} ms | {} KiB |".format(
        fields["parser"], fields["seeds"], fields["inputs"], fields["ok"], fields["rejected"],
        fields["crash"], fields["timeout"], fields["oom"], fields["silent"],
        fields["error_success"], fields["bad_exception"], fields["slowest_ms"],
        fields["peak_kib"]))
    for key in totals:
        totals[key] += int(fields[key])
lines.append("| **合计** | — | **{}** | {} | {} | {} | {} | {} | {} | {} | {} | — | — |".format(
    totals["inputs"], totals["ok"], totals["rejected"], totals["crash"], totals["timeout"],
    totals["oom"], totals["silent"], totals["error_success"], totals["bad_exception"]))
table = "\n".join(lines)

text = FINDINGS.read_text()
text = text.replace("<!--COVERAGE-->", table)
FINDINGS.write_text(text)
print(table)
print()
print("inputs:", totals["inputs"], "failures:",
      totals["crash"] + totals["timeout"] + totals["oom"] + totals["silent"] +
      totals["error_success"] + totals["bad_exception"])
