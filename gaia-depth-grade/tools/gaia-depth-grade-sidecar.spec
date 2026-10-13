# -*- mode: python ; coding: utf-8 -*-
# PyInstaller spec for the frozen gaia-depth-grade sidecar (one-file binary).
#
# The binary ships as a PixInsight update package (bin/gaia-depth-grade-sidecar)
# from repository/, which raw.githubusercontent.com serves only below GitHub's
# 100 MB per-file limit -- so the bundle carries only what the sidecar runs.
# What it needs was measured, not guessed: run-time import and file-open traces
# of every subcommand the PJSR scripts use (footprint, prepare offline+online,
# preview, render, nebula, grade). Sidecar 1.0.8, the first slim build, produced
# output bit-identical to 1.0.7 on a real master (100.8 MB -> ~55 MB binary).
# tools/build-sidecar.sh smoke-tests every offline subcommand on the build host
# and in bare Ubuntu 22.04 / Debian 12 / Rocky 9 images, so a module missing
# from the bundle fails the build.
#
# Hidden imports and compiled extensions (erfa, scipy, numpy) come from
# PyInstaller's hooks, which fire from the imports Analysis discovers in the cli
# chain; tools/pyinstaller-hooks/ replaces the two contrib hooks that bundled
# all of astropy (and needed matplotlib at build time) and the IERS tables.
import os

from PyInstaller.utils.hooks import collect_data_files, copy_metadata

HERE = os.path.dirname(os.path.abspath(SPEC))

datas = []
# astroquery: only the root package files (CITATION etc.) and the Gaia/TAP
# modules the sidecar imports. Every other service's data and all test data
# (~10 MB compressed) stay out.
datas += collect_data_files("astroquery", includes=["*.cfg", "CITATION", "gaia/**", "utils/**"],
                            excludes=["**/tests/**"])
datas += collect_data_files("photutils", excludes=["**/tests/**"])
# photutils (and friends) introspect their own dist-info at import via
# importlib.metadata.requires(...) -- PyInstaller bundles code but not metadata
# unless told, so copy each package's *.dist-info or the import aborts with
# PackageNotFoundError.
for pkg in ("photutils", "astroquery"):
    datas += copy_metadata(pkg)

# Never imported at run time by any subcommand (traced), so excluded outright;
# an import of any of them would raise ImportError loudly, never degrade.
EXCLUDES = [
    # GUI / dev / plotting (astropy only probes for matplotlib: optional)
    "tkinter", "matplotlib", "PyQt5", "PyQt6", "PySide2", "PySide6", "IPython",
    "pytest", "setuptools", "pkg_resources", "pydoc_data",
    # astroquery pulls these for authenticated archive logins (keyring backends),
    # which the anonymous Gaia query never uses: 4.6 MB of OpenSSL bindings.
    "cryptography", "keyring", "jeepney", "secretstorage", "cffi", "_cffi_backend",
    # astroquery's HTML scraping and VO-client dependencies; the Gaia TAP client
    # uses neither.
    "bs4", "soupsieve", "html5lib", "webencodings", "pyvo",
    # scipy subpackages no code path reaches (the sidecar uses ndimage + spatial,
    # which pull in linalg/sparse/special)
    "scipy.optimize", "scipy.stats", "scipy.interpolate", "scipy.integrate",
    "scipy.signal", "scipy.fft", "scipy.fftpack", "scipy.io", "scipy.odr",
    "scipy.cluster", "scipy.datasets", "scipy.differentiate", "scipy.misc",
    # Pillow: the sidecar writes PNG only. These codecs/extensions carry the
    # bundled AVIF/WebP/LCMS/FreeType libraries.
    "PIL._avif", "PIL.AvifImagePlugin", "PIL._webp", "PIL.WebPImagePlugin",
    "PIL._imagingcms", "PIL.ImageCms", "PIL._imagingft", "PIL.ImageFont",
    "PIL._imagingtk", "PIL.ImageTk", "PIL._imagingmath", "PIL.ImageMath",
    "PIL._imagingmorph", "PIL.ImageMorph", "PIL.ImageQt",
]

a = Analysis(
    [os.path.join(HERE, "sidecar_entry.py")],
    pathex=[],                       # gaia_depth_grade resolves via the venv install
    binaries=[],
    datas=datas,
    hiddenimports=[],
    hookspath=[os.path.join(HERE, "pyinstaller-hooks")],
    hooksconfig={},
    runtime_hooks=[],
    excludes=EXCLUDES,
    noarchive=False,
)

# Base system libraries come from the user's system, never from the build
# machine. The wheels link them under the manylinux policy (libstdc++ and
# libgcc_s are on its system whitelist; Pillow also needs libz), and every
# distribution that runs PixInsight has them -- PixInsight's own core needs
# libstdc++ GLIBCXX_3.4.30. Bundling the build host's copies is what kept
# sidecar 1.0.7 from starting on Ubuntu 22.04 / Debian 12 / Rocky 9.
SYSTEM_LIBS = {"libstdc++.so.6", "libgcc_s.so.1", "libz.so.1"}
a.binaries = [b for b in a.binaries if os.path.basename(b[0]) not in SYSTEM_LIBS]


def _write_third_party_notices(path):
    """Licence texts of everything bundled: each Python distribution with code or
    data in the bundle (its dist-info licence files -- numpy, scipy and Pillow's
    also cover the native libraries their wheels vendor), the CPython licence,
    and the PyInstaller bootloader's licence."""
    import importlib.metadata as md
    import sys

    tops = {name.split(".")[0] for name, *_ in a.pure}
    for dest, *_ in list(a.binaries) + list(a.datas):
        first = dest.replace("\\", "/").split("/")[0]
        tops.add(first[:-5] if first.endswith(".libs") else first.split(".")[0])
    by_top = md.packages_distributions()
    dists = {}
    for top in tops:
        for name in by_top.get(top, []):
            d = md.distribution(name)
            dists[d.metadata["Name"].lower()] = d
    dists.pop("gaia-depth-grade", None)     # this product itself
    dists.pop("pyinstaller", None)          # build tool; its bootloader is listed below

    def licence_texts(d):
        out = []
        for f in sorted(d.files or [], key=str):
            parts = str(f).split("/")
            base = parts[-1].upper()
            if parts[0].endswith(".dist-info") and (
                    "licenses" in parts or base.startswith(("LICEN", "COPYING", "NOTICE", "AUTHORS"))):
                out.append((str(f), d.locate_file(f).read_text(encoding="utf-8", errors="replace")))
        return out

    sections = []
    for key in sorted(dists):
        d = dists[key]
        meta = d.metadata
        lic = meta.get("License-Expression") or meta.get("License") or "see licence text"
        texts = licence_texts(d)
        if not texts:
            raise SystemExit(f"no licence file found for bundled distribution {meta['Name']} {d.version}")
        body = "\n\n".join(f"--- {name} ---\n{text.strip()}" for name, text in texts)
        sections.append(f"{meta['Name']} {d.version}\nLicence: {lic.splitlines()[0][:200]}\n\n{body}")
    # The interpreter: python-build-standalone, whose libpython statically links
    # every stdlib extension's library. Licence texts come from its full archive
    # (tools/cpython-licenses.py, run by build-sidecar.sh); the ones for _tkinter
    # (Tcl/Tix/X11) and _dbm (Berkeley DB) are left out because those modules are
    # separate files the bundle does not contain, and OpenSSL 1.1's because the
    # build links OpenSSL 3. Its zstd licence is missing from that archive, so it
    # is vendored (tools/licenses/LICENSE.zstd.txt, zstd v1.5.7).
    cpy = os.environ.get("GDG_CPYTHON_LICENSES")
    if not cpy or not os.path.isfile(os.path.join(cpy, "licenses", "LICENSE.cpython.txt")):
        raise SystemExit("GDG_CPYTHON_LICENSES must name the output of tools/cpython-licenses.py "
                         "(build with tools/build-sidecar.sh)")
    skip = {"LICENSE.tcl.txt", "LICENSE.tix.txt", "LICENSE.libX11.txt", "LICENSE.libXau.txt",
            "LICENSE.libxcb.txt", "LICENSE.bdb.txt", "LICENSE.openssl-1.1.txt"}
    files = sorted(f for f in os.listdir(os.path.join(cpy, "licenses")) if f not in skip)
    files.remove("LICENSE.cpython.txt")
    lic_paths = [os.path.join(cpy, "licenses", "LICENSE.cpython.txt")]
    lic_paths += [os.path.join(cpy, "licenses", f) for f in files]
    lic_paths.append(os.path.join(HERE, "licenses", "LICENSE.zstd.txt"))
    body = "\n\n".join(f"--- {os.path.basename(p)} ---\n" + open(p, encoding="utf-8", errors="replace").read().strip()
                       for p in lic_paths)
    sections.append(f"CPython {sys.version.split()[0]} (the bundled interpreter, python-build-standalone) and the "
                    "libraries statically linked into it\n\n" + body)
    pyi = md.distribution("pyinstaller")
    boot = [t for n, t in licence_texts(pyi)]
    sections.append(f"PyInstaller {pyi.version} bootloader (the program's start-up code)\n\n" + "\n\n".join(boot).strip())
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("Gaia Depth Grade sidecar -- third-party software notices\n"
                 "========================================================\n\n"
                 "The sidecar program (bin/gaia-depth-grade-sidecar) is a frozen Python application.\n"
                 "It contains the following third-party software, under the licences reproduced below.\n"
                 "The shared base system libraries (libc, libm, libstdc++.so.6, libgcc_s.so.1, libz.so.1)\n"
                 "are not included: the program uses the ones installed on your system.\n\n")
        fh.write("\n\n".join("=" * 78 + "\n" + s for s in sections) + "\n")


_write_third_party_notices(os.path.join(DISTPATH, "THIRD_PARTY_NOTICES.txt"))

pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="gaia-depth-grade-sidecar",
    debug=False,
    bootloader_ignore_signals=False,
    # No strip: binutils' strip in the Rocky 9 build image corrupts the
    # auditwheel-patched OpenBLAS in numpy.libs ("ELF load command address/offset
    # not page-aligned"). Stripping would save ~8 MB; not worth a selective-strip
    # step while the package is well under GitHub's 100 MB limit.
    strip=False,
    upx=False,
    runtime_tmpdir=None,
    console=True,
)
