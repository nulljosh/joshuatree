#!/usr/bin/env python3
"""Host-only CI fixtures: ARM checks packed across the ARM shards only, and fail-fast path validation."""
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
        "retry|3|ARM first|python3 tools/checks/arm64-one-check.py\n"
        "retry|6|ARM TLS|python3 tools/checks/arm64-two-check.py --tls\n"
        "once |8|Host first|python3 host.py\n"
        "once |7|Host unknown|python3 unknown.py\n"
    )
    suite.write_text(original)
    # what makes a check ARM is what it runs, not where it sits: fixtures that do and do not start the ARM QEMU
    (checks / "arm64-one-check.py").write_text('subprocess.run(["qemu-system-aarch64", "-machine", "raspi4b"])\n')
    (checks / "arm64-two-check.py").write_text('subprocess.run(["make", "-s", "-C", arch, "kernel8.elf"])\n')
    (root / "host.py").write_text("print('host')\n")
    (root / ".github/workflows").mkdir(parents=True)
    shutil.copy(ROOT / ".github/workflows/check.yml", root / ".github/workflows/check.yml")
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
    arm = {"6", "8", "9", "10"}   # the raspi4b runner's shards (check.yml)
    assert all(row[1] in arm for row in rows[:2]), "an ARM check (by what it runs) is off the ARM shards"
    assert rows[0][1] != rows[1][1], "ARM checks were not spread across the ARM shards"
    assert all(row[1] not in arm for row in rows[2:]), "a host check (from an ARM shard) stayed on one"
    assert rows[2][1] != rows[3][1], "ordinary checks did not balance"
    assert re.sub(r"\|\d+\|", "|SHARD|", suite.read_text()) == re.sub(
        r"\|\d+\|", "|SHARD|", original), "changed check commands or retry modes"
    invalid = subprocess.run(cmd + ["1", "12"], cwd=root, env=env, capture_output=True)
    assert invalid.returncode != 0, "accepted a shard count check.yml does not run"

    # one list: the ARM shards and the shard count in check.yml are what ci-local.sh and the suite's SHARDS use
    wf = (ROOT / ".github/workflows/check.yml").read_text()
    arm_wf = re.search(r"contains\(fromJSON\('\[([0-9, ]+)\]'\), matrix\.shard\)", wf).group(1).replace(",", "").split()
    k_wf = len(re.search(r"shard: \[([0-9, ]+)\]", wf).group(1).split(","))
    assert all(f"contains(fromJSON('[{', '.join(arm_wf)}]'), matrix.shard)" in l for l in wf.splitlines() if "fromJSON(" in l), "check.yml spells the ARM list two ways"
    local = (ROOT / "tools/ci-local.sh").read_text()
    assert f'"{" ".join(arm_wf)}"' in local, "ci-local.sh's ARM group is not check.yml's"
    assert local.count(f"SHARDS={k_wf} ") >= 1 and f"SHARDS={k_wf} " in wf, "SHARDS differs from check.yml's matrix"

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

print("PASS: ARM by what a check runs, spread across the ARM shards, host checks off them, one ARM list and shard count, dry run, fail-fast preflight")
