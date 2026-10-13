"""Extract the licence texts of the bundled CPython build.

The sidecar's interpreter is python-build-standalone (PBS), whose libpython
statically links OpenSSL, libffi, SQLite, xz, bzip2, mpdecimal, expat, libuuid,
libedit, ncurses, zlib and zstd. Only PBS's "full" archive carries their licence
texts (python/licenses/) and the map of what is linked (python/PYTHON.json);
the install-only interpreter uv runs does not. build-sidecar.sh downloads that
archive once (sha256-pinned) and runs this inside the build image:

    python -I cpython-licenses.py <pbs-full.tar.zst> <out_dir>

writing <out_dir>/PYTHON.json and <out_dir>/licenses/*. Needs Python >= 3.14
(compression.zstd).
"""
import os
import sys
import tarfile
from compression import zstd

archive, out = sys.argv[1], sys.argv[2]
os.makedirs(os.path.join(out, "licenses"), exist_ok=True)
n = 0
with zstd.open(archive, "rb") as raw, tarfile.open(fileobj=raw, mode="r|") as tf:
    for m in tf:
        if not m.isfile():
            continue
        if m.name == "python/PYTHON.json":
            dest = os.path.join(out, "PYTHON.json")
        elif m.name.startswith("python/licenses/") and "/" not in m.name[len("python/licenses/"):]:
            dest = os.path.join(out, "licenses", os.path.basename(m.name))
        else:
            continue
        with tf.extractfile(m) as src, open(dest, "wb") as dst:
            dst.write(src.read())
        n += 1
if not os.path.exists(os.path.join(out, "PYTHON.json")) or n < 2:
    sys.exit(f"{archive}: no python/PYTHON.json or python/licenses/ found")
print(f"extracted {n - 1} licence files + PYTHON.json")
