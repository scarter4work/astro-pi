"""
Command-line entry point wiring the eleven passes into one tool.

Every subcommand shares a `--db` flag (defined once on a common parent
parser so it can be given right after the subcommand name, matching how
every other flag here works) and otherwise takes only what it needs:
`--root` on `scan`, `--limit` on `solve`/`measure`, `--out-dir` on
`manifest`.

Errors from the underlying passes (a missing archive root, a missing
astap_cli install, a refusal to mark everything missing from an empty
scan) are never caught here. They are deliberate, loud failures --
letting them propagate as an uncaught exception (non-zero exit, real
traceback) is the correct behaviour, not a gap. Only argparse's own
usage errors (bad flags, unknown subcommand) are translated into a
plain non-zero return, since those are user-input mistakes rather than
archive-state problems.

There is deliberately no `quarantine` subcommand: nothing here may
touch or move FITS data.
"""
import argparse
import sys
from pathlib import Path

from . import cluster, db, disposition, grouping, inventory, manifest, quality, solve
from .config import Config


def _build_parser() -> argparse.ArgumentParser:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--db", required=True,
                         help="path to the sqlite metadata store")

    parser = argparse.ArgumentParser(prog="astrometa")
    sub = parser.add_subparsers(dest="command", required=True)

    scan_p = sub.add_parser(
        "scan", parents=[common],
        help="walk archive roots, inventory frames, then mark absent "
             "frames missing")
    scan_p.add_argument(
        "--root", action="append", default=[],
        help="root directory to walk (repeatable); defaults to the "
             "configured archive and live roots")

    sub.add_parser(
        "cluster", parents=[common],
        help="cluster unassigned light frames into fields")

    solve_p = sub.add_parser(
        "solve", parents=[common],
        help="plate-solve one representative frame per unsolved field")
    solve_p.add_argument("--limit", type=int, default=None,
                          help="solve at most this many fields")

    measure_p = sub.add_parser(
        "measure", parents=[common],
        help="measure HFD/star-count quality for unmeasured light frames")
    measure_p.add_argument("--limit", type=int, default=None,
                            help="measure at most this many frames")

    sub.add_parser(
        "group", parents=[common],
        help="rebuild projects from light frames")

    manifest_p = sub.add_parser(
        "manifest", parents=[common],
        help="export a manifest sidecar for every leaf directory")
    manifest_p.add_argument(
        "--out-dir", default=None,
        help="mirror manifests under this directory instead of writing "
             "beside the frames -- required when the archive is mounted "
             "read-only")

    sub.add_parser(
        "status", parents=[common],
        help="print row counts for the core tables")

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        # argparse's own usage errors (bad flags, unknown subcommand,
        # --help) -- a plain non-zero return, not a pass failure.
        return exc.code if isinstance(exc.code, int) else 2

    cfg = Config()
    conn = db.connect(Path(args.db))
    db.init_schema(conn)

    if args.command == "scan":
        configured = [cfg.archive_root, cfg.live_root]
        roots = ([Path(r) for r in args.root] if args.root else configured)
        res = inventory.scan(conn, roots)

        # The whole-database missing sweep runs ONLY when this scan
        # covered the full configured root set. `mark_missing` flips
        # every 'present' frame absent from seen_hashes, so running it
        # after `scan --root /archive/astro_data/2026-09-08` would flip
        # the other ~34,000 frames to 'missing' purely because the walk
        # never reached them. mark_missing's own guard cannot catch
        # that: it only refuses a COMPLETELY empty seen_hashes, and a
        # one-night scan returns a perfectly healthy non-empty set.
        #
        # `missing` is how a deliberate cull gets recorded -- the single
        # signal this project exists to make trustworthy -- so
        # fabricating it from a partial walk corrupts the store's whole
        # purpose. Coverage is a set comparison on resolved paths, and
        # deliberately conservative: a --root that happens to be a
        # PARENT of the configured roots is not recognised as covering
        # them and skips the sweep too. Skipping is always the safe
        # direction, and it is never silent -- the skip and its reason
        # are printed where the count would otherwise be.
        covers_configured = ({r.resolve() for r in roots}
                             >= {r.resolve() for r in configured})
        if covers_configured:
            missing_txt = f"missing={disposition.mark_missing(conn, res.seen_hashes)}"
        else:
            missing_txt = (
                f"missing=skipped (--root narrowed this scan to "
                f"{len(roots)} root(s); the whole-database missing "
                f"sweep runs only when the scan covers the configured "
                f"archive and live roots, so frames outside the "
                f"scanned roots are NOT marked missing)")
        print(f"added={res.added} updated={res.updated} "
              f"failed={res.failed} {missing_txt} "
              f"(failed overlaps added/updated: a frame that fails a "
              f"read is still inventoried, with its failure recorded)")
    elif args.command == "cluster":
        created = cluster.assign_fields(conn)
        print(f"fields_created={created}")
    elif args.command == "solve":
        solved = solve.solve_fields(conn, cfg, args.limit)
        print(f"solved={solved}")
    elif args.command == "measure":
        res = quality.measure_frames(conn, cfg, args.limit)
        print(f"measured={res.measured} failed={res.failed}")
    elif args.command == "group":
        projects = grouping.build_projects(conn)
        print(f"projects={projects}")
    elif args.command == "manifest":
        out_dir = Path(args.out_dir) if args.out_dir else None
        manifests = manifest.export_all(conn, out_dir=out_dir)
        print(f"manifests={manifests}")
    elif args.command == "status":
        for table in ("frames", "fields", "objects", "projects"):
            n = conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
            print(f"{table}: {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
