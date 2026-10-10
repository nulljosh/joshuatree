#!/usr/bin/env python3
"""Host-only CI fixtures: reserved ARM builds and fail-fast path validation."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]

with tempfile.TemporaryDirectory(prefix="ci-tooling-") as tmp:
    root = Path(tmp)
    checks = root / "tools/checks"
    checks.mkdir(parents=True)
    suite = checks / "ci-suite.sh"
    original = (
        "retry|6|ARM first|python3 tools/checks/arm64-one-check.py\n"
        "retry|6|ARM TLS|python3 tools/checks/arm64-two-check.py --tls\n"
        "once |7|Host first|python3 host.py\n"
        "once |7|Host unknown|python3 unknown.py\n"
    )
    suite.write_text(original)
    bindir = root / "bin"
    bindir.mkdir()
    gh = bindir / "gh"
    gh.write_text(f"#!{sys.executable}\n" + '''import json, sys
if sys.argv[1] == "run":
    print(json.dumps({"jobs": [{"name": "suite (0)", "databaseId": 1}]}))
else:
    print("timestamp PASS   ARM first (100s)\\ntimestamp PASS   ARM TLS (200s)\\ntimestamp PASS   Host first (40s)")
''')
    gh.chmod(0o755)
    env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ["PATH"])
    cmd = [sys.executable, str(ROOT / "tools/gen/ci-balance.py")]
    dry = subprocess.run(cmd + ["--check", "1"], cwd=root, env=env,
                         text=True, capture_output=True, check=True)
    assert suite.read_text() == original, "--check changed the manifest"
    subprocess.run(cmd + ["1"], cwd=root, env=env, check=True)
    rows = [line.split("|") for line in suite.read_text().splitlines()]
    assert [row[1] for row in rows[:2]] == ["6", "6"], "ARM checks separated"
    assert all(row[1] != "6" for row in rows[2:]), "pinned work omitted from loads"
    assert rows[2][1] != rows[3][1], "ordinary checks did not balance"
    assert re.sub(r"\|\d+\|", "|SHARD|", suite.read_text()) == re.sub(
        r"\|\d+\|", "|SHARD|", original), "changed check commands or retry modes"
    invalid = subprocess.run(cmd + ["1", "6"], cwd=root, env=env, capture_output=True)
    assert invalid.returncode != 0, "accepted too few shards"

    shutil.copy(ROOT / "tools/ci-local.sh", root / "tools/ci-local.sh")
    (root / "tools/ci-lock.sh").write_text("ci_lock_acquire() { return 0; }\n")
    shutil.copy(ROOT / "tools/checks/tmp-paths-check.py", checks)
    (checks / "tmp-paths-baseline.txt").write_text("")
    (checks / "bad-check.py").write_text('path = "' + '/tmp/' + 'jt-collision"\n')
    rejected = subprocess.run(["bash", "tools/ci-local.sh"], cwd=root,
                              text=True, capture_output=True, timeout=10)
    assert rejected.returncode == 1, rejected.stdout + rejected.stderr
    assert "FAIL: tmp-paths-check" in rejected.stdout, "guard did not run"
    assert "-- build --" not in rejected.stdout, "build ran after failed preflight"

print("PASS: ARM affinity, load accounting, dry run, shard bounds and fail-fast preflight")
