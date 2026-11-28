# Benchmark results and raw archives

This directory keeps the reports, plots, summaries, source/build provenance and small supporting records used to review Angaria's performance. Large raw samples, compressed attempt archives and profiler dumps are retained in a separate maintainer archive. The [archive catalog](archive-catalog.json) identifies every payload removed from this checkout by original path, archive-relative path, byte count and SHA-256 hash.

The first archive is **`2026-11-28-332118be2291`**, captured from repository commit **`332118be2291264b4b64dcc53212e35c4d3998fa`**. It preserves all 26 result collections, including failed attempts, scheduling outliers and unfavorable measurements. Moving the evidence does not change any published findings or original measurement manifests.

## Archive layout

The archive root is configured locally; its private NAS location is not recorded in Git. Its layout is:

```text
<archive-root>/
  archive.json
  SHA256SUMS
  results/
    milestone-4-fork-ownership/
      ...original complete result tree...
    ...other result collections...
  repository/
    angaria.bundle
```

`archive.json` and `SHA256SUMS` describe the preserved snapshot. The checked-in catalog links the omitted payloads to this archive and retains hashes for its index and Git bundle. The bundle preserves the locally available source commits referenced by benchmark manifests. Temporary CI merge identifiers that were already absent locally are not recovered by this backup; the original provenance records remain intact. It can be checked with `git bundle verify` and used to create a separate historical checkout when a study requires its recorded source or renderer.

The raw archive is maintainer-managed and is not downloadable from this public repository. Reading the published reports and plots needs no archive access. Independently reconstructing their historical measurements requires the corresponding raw evidence; collecting a new study remains available through the documented benchmark commands.

## Restore archived evidence

Restore only the omitted files listed in the catalog. Keep their original relative paths and verify their recorded sizes and SHA-256 hashes. Do not copy the complete old result tree over this checkout: that could overwrite updated reports, archive notices or scripts.

Several historical figure scripts use their directory location and import sibling study helpers. Restore into the original `benchmarks/results/<collection>/...` layout in this checkout or a disposable worktree, then run the study's documented commands. Running the copied NAS scripts directly from another directory layout is not a universal substitute.

From the repository root, set `ANGARIA_BENCHMARK_ARCHIVE` to the local archive root shown above. The following standalone command restores one collection. It stages each copy, verifies its bytes, refuses to replace a different existing file, and publishes only catalog-listed payloads. Existing matching payloads are left in place; retained reports and documentation are not copied.

```sh
export ANGARIA_BENCHMARK_ARCHIVE=/path/to/2026-11-28-332118be2291
python3 -B - milestone-4-fork-ownership <<'PY'
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

archive = Path(os.environ["ANGARIA_BENCHMARK_ARCHIVE"]).expanduser().resolve(strict=True)
root = Path.cwd().resolve()
catalog = json.loads((root / "benchmarks/results/archive-catalog.json").read_text())
prefix = "benchmarks/results/" + sys.argv[1] + "/"
entries = [item for item in catalog["files"] if item["path"].startswith(prefix)]
if not entries:
    raise SystemExit("No archived payloads for that collection")

def matches(path, item):
    if path.is_symlink() or not path.is_file() or path.stat().st_size != item["bytes"]:
        return False
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest() == item["sha256"]

for item in entries:
    source = archive / item["archive_path"]
    target = root / item["path"]
    if not source.resolve().is_relative_to(archive) or not target.resolve().is_relative_to(root):
        raise SystemExit("Catalog path leaves its archive or checkout")
    if target.exists() or target.is_symlink():
        if not matches(target, item):
            raise SystemExit("Refusing to overwrite: " + item["path"])
        continue
    target.parent.mkdir(parents=True, exist_ok=True)
    staged = None
    try:
        with tempfile.NamedTemporaryFile(dir=target.parent, prefix=".archive-restore-", delete=False) as temporary:
            staged = Path(temporary.name)
            with source.open("rb") as stream:
                shutil.copyfileobj(stream, temporary)
        if not matches(staged, item):
            raise SystemExit("Archive size/hash mismatch: " + item["archive_path"])
        os.link(staged, target)  # Fails rather than replacing a concurrently created file.
    finally:
        if staged is not None:
            staged.unlink(missing_ok=True)
print("Restored or verified", len(entries), "payloads for", sys.argv[1])
PY
```

A restored payload remains ignored by Git. The restore operation checks preservation and does not establish that a historical workload is valid against a later framework version. Use the recorded source/renderer when required; the original Git bundle preserves those revisions.

After restoring the example collection, use its existing strict checks:

```sh
python3 -B benchmarks/ownership_performance.py verify benchmarks/results/milestone-4-fork-ownership/linux-x86_64-confirmation
python3 -B benchmarks/ownership_performance.py render benchmarks/results/milestone-4-fork-ownership/linux-x86_64-confirmation
```

These commands reconstruct metrics from raw attempts and compare them with the published records before plotting. A summary or existing image alone is not independently verified raw evidence. Other studies retain their own commands in their report indexes; this guide does not weaken their checks.

## Retention for new studies

Collect complete evidence outside tracked result directories, or in ignored build output. Keep every attempt, including failures and regressions. Verify a durable external copy before publishing the small reports, plots, summaries, provenance and archive references in Git. GitHub Actions artifacts provide temporary transfer storage and currently expire after 14 days; they are not the permanent archive.

Removing captured files from the current tree reduces checkout size and future growth. It does not remove their objects from existing Git history. History rewriting is a separate operation and is not part of this archive cleanup.
