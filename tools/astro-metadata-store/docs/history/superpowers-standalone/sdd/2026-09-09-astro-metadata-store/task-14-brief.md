### Task 14: First real run against the archive

**Files:**
- Create: `docs/runbook.md`
- No source changes

**Interfaces:**
- Consumes: the `astrometa` console script
- Produces: a populated `store.sqlite` and a written record of the first run's numbers

This task is deliberately last and deliberately manual: it is where the store meets 28,135 real files and where any number that disagrees with the spec gets investigated rather than explained away.

- [ ] **Step 1: Deploy to scott-server**

```bash
ssh root@192.168.68.53 'pct list'          # choose or create the LXC
# bind-mount the two sources read-only into the container:
#   pct set <id> -mp0 /data/backups/qnap,mp=/archive,ro=1
#   pct set <id> -mp1 /mnt/qnap-source,mp=/live,ro=1
```

- [ ] **Step 2: Run the inventory pass**

```bash
astrometa scan --db /data/astro-metadata/store.sqlite
```

Expected: `added` close to 28,135. Record the exact number.

- [ ] **Step 3: Reconcile against the known count**

The workstation and scott-server previously reported 28,135 vs 28,123 frames for the same archive. Compare `added + failed` against both. Investigate the difference by hash rather than choosing whichever number is convenient — this discrepancy is a recorded open item in the spec, and closing it is part of this task.

- [ ] **Step 4: Cluster and solve a sample**

```bash
astrometa cluster --db /data/astro-metadata/store.sqlite
astrometa solve   --db /data/astro-metadata/store.sqlite --limit 20
```

Verify a known field: the IC 1848 frames must solve to approximately
RA 42.8625, Dec 60.0697.

- [ ] **Step 5: Record results and commit the runbook**

Write `docs/runbook.md` capturing: the real frame count, the number of distinct fields, solve success rate, wall-clock per pass, and any frame that failed to read. Then:

```bash
git add docs/runbook.md
git commit -m "docs: first full archive run results"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| §3 Decisions — SQLite on local ZFS | 1 |
| §4 Architecture — read-only bind mounts | 14 |
| §5 Three keys — content hash, WCS, fingerprint | 4, 7 |
| §6 Pipeline 1 Inventory | 5 |
| §6 Pipeline 2 Classify | 3 |
| §6 Pipeline 3 Headers | 2 |
| §6 Pipeline 4 Fingerprint | 4 |
| §6 Pipeline 5 Cluster by pointing | 6 |
| §6 Pipeline 6 Solve | 7 |
| §6 Pipeline 7 Name | 9 |
| §6 Pipeline 8 Measure quality | 8 |
| §6 Pipeline 9 Group | 10 |
| §7 Data model | 1 |
| §7 Confidence model | 9 |
| §8 Disposition, quarantine, thresholds, backfill | 11 |
| §9 Error handling | 5 (read errors), 7 (solve attempts), 11 (missing), 14 (count reconciliation) |
| §10 Testing | every task |
| §11 Non-goals | enforced by Global Constraints |

**Gap found and closed:** the spec's manifest durability requirement (§3) had no task in the first draft; it is now Task 12.

**Deferred deliberately:** SIMBAD network resolution is scaffolded in Task 9 (`upsert_object` accepts `simbad_id`/`ra`/`dec`) but the live query is not implemented, because it needs the first real solve results from Task 14 to calibrate the search radius. It is the first item of the follow-up plan, not a silent omission.

**Type consistency:** `content_hash` is a hex `str` everywhere; `fingerprint` is a hex `str` everywhere and compared only via `imagekeys.hamming`; `field_id` is `int`; disposition values are exactly `present` / `missing` / `quarantined` in Tasks 1, 5, 11 and 12.
