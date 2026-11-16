# Vendored source archives

NukeX's CMake build takes every third-party source it fetches from this
directory, never from the network. `cmake/NukeXVendoredArchives.cmake` declares
each one with FetchContent as `URL <this dir>/<file>` +
`URL_HASH SHA256=<hash from SHA256SUMS>`; CMake refuses an archive whose hash
differs, and a missing archive or `SHA256SUMS` entry stops the configure. There
is no download fallback.

Each file is an unmodified upstream download, renamed only. `verify-upstream.sh`
downloads them again (it is the one script here that uses the network, and
nothing in the build runs it) and checks that the committed bytes match those
downloads and that their contents match the upstream release tags.

| File | Upstream download | Contents |
|---|---|---|
| `cfitsio-4.6.3.tar.gz` (4.7 MB) | `https://github.com/HEASARC/cfitsio/archive/refs/tags/cfitsio-4.6.3.tar.gz` | The whole tree of tag `cfitsio-4.6.3` (commit `386e719ccc5e30ca497ec2df57c4ef1de5adbc3f`), identical to `git archive cfitsio-4.6.3`. Licence: `licenses/License.txt`. |
| `nlohmann_json-v3.11.3.tar.xz` (111 KB) | `https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz` | Upstream's own FetchContent release asset: 53 files of tag `v3.11.3` (commit `9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03`), each byte-identical to the tag's file. Upstream left out the tests, docs and tooling; the headers, `CMakeLists.txt` and `cmake/` the build uses are all there. Licence: `LICENSE.MIT`. |
| `sqlite-amalgamation-3460100.zip` (2.8 MB) | `https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip` | SQLite 3.46.1 amalgamation; SHA3-256 `af6aae8d3eccc608857c63cf56efbadc70da48b5c719446b353ed88dded1e288`, the hash NukeX pinned when it downloaded this zip from sqlite.org. SQLite is in the public domain (see the header of `sqlite3.c`). |

To change a version: download the new upstream file here, replace its line in
`SHA256SUMS` (`sha256sum <file>`), update the file name where it is declared
(`CMakeLists.txt`, `src/lib/io/CMakeLists.txt`, `src/lib/learning/CMakeLists.txt`,
`test/standalone_io/CMakeLists.txt`), and add its source and tag to
`verify-upstream.sh` and this table.
