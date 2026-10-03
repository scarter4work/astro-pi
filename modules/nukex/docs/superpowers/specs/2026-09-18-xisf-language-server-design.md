# XISF Language Server — Design Seed

> **Status:** PARKED design, not scheduled. This is **not** part of the
> `2026-09-05-v5-outstanding-program.md` plan and must not be folded into its
> single-re-baseline ordering — it moves no pixels and touches no golden. It is
> captured here as a real spec (per the project's no-stub / no-TODO rule) so it
> is actionable when picked up, rather than as a placeholder that rots.
>
> **Trigger:** Pleiades published **XISF 1.0 Specification, Revision 1**
> (document version 1.01, September 2026) alongside the **PixInsight 1.9.5
> "Lockhart"** release. It is the first update to the XISF spec in ~9 years and,
> for the first time, ships a **formal XML Schema** — which makes a schema-aware
> language server tractable where before it would have been reverse-engineered.

## 1. Motivation

XISF is NukeX's native output container (`src/lib/io`). Until Rev 1 the format
was specified only in prose; validating that what we write is conformant meant
round-tripping through PixInsight by hand. Rev 1 changes two things that matter
to us directly:

- **A machine-readable schema now exists.** The spec references an XSD at
  `http://pixinsight.com/xisf/xisf-1.0.xsd`. A language server can validate
  headers against it instead of against a human's memory of the prose.
- **New standard property namespaces and codecs** (see §3). NukeX's writer must
  emit them correctly, and older readers (including third-party Python XISF
  libraries — see the known keyword-loss trap) may not yet understand them. A
  server that knows the exact Rev-1 surface catches both a producer bug (we
  wrote a non-conformant header) and a consumer risk (we chose a codec the
  target reader can't decode) before a file leaves the build.

This mirrors the two existing dev-tooling language servers Scott already runs as
MCP servers — the **PCL parser** (`pcl_class_info`, `pcl_template`,
`pcl_validate`, `pcl_completions`) and the **PJSR parser** (`pjsr-analyze`,
`pjsr-class`, `pjsr-help`, `pjsr-template`), both under `~/projects/EZ-suite-bsc/`.
An XISF server is the natural third member: PCL for module C++, PJSR for scripts,
XISF for the data files those produce.

## 2. What it is (scope)

A schema- and spec-aware server for the XISF **container format** — the XML
header and its data-block declarations — **not** an image-pixel tool. Proposed
capabilities, named to rhyme with the PCL/PJSR servers:

- `xisf_validate` — validate an XISF header (or a `.xisf` monolithic file's
  header segment) against `xisf-1.0.xsd` **plus** the Rev-1 conformance
  requirements the bare XSD does not encode (e.g. codec/byte-order/checksum
  rules from §3). Loud, specific errors — never "looks fine."
- `xisf_property_info <namespace.name>` — describe a standard property,
  including the **new namespaces** (esp. `AstrometricSolution`, and the
  color-space transform properties). Reports type, units, cardinality, and
  which spec section defines it.
- `xisf_header_template <kind>` — emit a minimal conformant header skeleton
  (mono image, RGB image, with/without an embedded astrometric solution) using
  the recommended codec.
- `xisf_codec_info` — the codec/checksum matrix: `zlib`, `lz4`, `lz4hc`,
  `zstd`, `zstd+sh` (byte-shuffled), the sub-block format, and SHA-1/256/512
  checksum rules — with a "will reader X decode this?" advisory.
- `xisf_completions` — element/attribute/property-name completion for an
  in-progress header.

Source of truth (pin these in the server's fetch/cache layer):
- Spec: `https://pixinsight.net/dev/index.php?articles/xisf-1-0-specification-revision-1.22/`
- Schema: `http://pixinsight.com/xisf/xisf-1.0.xsd`

## 3. The Rev-1 surface the server must cover

From the "Changes in Revision 1" section (v1.01, Sept 2026):

- **`AstrometricSolution` property namespace** — plate-solve / WCS solutions now
  have a formal standard storage location. **PixInsight 1.9.5 writes solutions
  in this format.** This is the single most NukeX-relevant addition: see §4.
- **Zstandard compression** — `zstd` and `zstd+sh` (with byte shuffling) added
  to the existing `zlib` / `lz4`. Zstandard is now the **recommended** codec.
  Spec defines the LZ4 block format, Zstandard frames, compression sub-blocks,
  and the byte-shuffling scheme.
- **Color-space transformations** — RGB, CIE XYZ, CIE L\*a\*b\*.
- **Formal XML Schema**, **conformance requirements**, and an
  **extension-elements framework** (so the server must tolerate, not reject,
  well-formed extension elements).
- Clarifications the server should encode as rules: string properties are
  **always UTF-8**; decoders must read data blocks in **both byte orders** and
  verify **SHA-1/256/512** block checksums; scalar plain-text serialization and
  property format specifiers; FITS-header-keyword handling.

## 4. Why this belongs near NukeX v5 specifically

NukeX's registration/alignment path (per-channel registration; chained
alignment, Task 6 of the outstanding program) computes geometry that maps
frames onto a reference. When NukeX writes a stacked result whose frames were
plate-solved, Rev 1 says the astrometric solution should live in the standard
`AstrometricSolution` namespace, not only as scattered FITS `CRVAL/CD` keywords.
Two forward items fall out (record them; do **not** implement here):

1. **Producer conformance:** audit `src/lib/io` XISF writer output with
   `xisf_validate` once the server exists; confirm any astrometric metadata we
   emit uses the Rev-1 namespace and that our declared codec/checksum attributes
   are conformant.
2. **Consumer risk:** if the writer adopts `zstd`/`zstd+sh`, gate it behind a
   check that downstream tooling (PixInsight ✓; third-party Python XISF readers
   ✗ until verified) can decode it. Default to `zlib`/`lz4` for files destined
   for the Python side of the pipeline until proven.

## 5. Open decisions (resolve before building — do not silently pick)

- **Home:** sibling MCP server under `~/projects/EZ-suite-bsc/` next to
  `pcl_parser`/`pjsr_parser` (consistent with the existing tooling suite), or a
  `tools/` component inside nukex5 (closer to the writer it validates)?
  Leaning EZ-suite-bsc for reuse across all PixInsight projects; nukex5 would
  then just *consume* it in a CI/preflight check.
- **Validation depth:** XSD-only (cheap, incomplete — the XSD can't express the
  codec/checksum/byte-order conformance rules), vs. XSD + a hand-written
  conformance pass. The value is mostly in the second layer.
- **Scheduling:** gated behind the v5 outstanding program and the
  manuscript-platform-first priority. Not FIRST UP.

## 6. First concrete step when picked up

Fetch and archive `xisf-1.0.xsd`, write a throwaway validator that runs a
NukeX-produced `.xisf` header through it, and record what the bare XSD does and
does **not** catch. That single experiment sizes the real work (how much of §2
is XSD-for-free vs. hand-written conformance) before any server scaffolding.
