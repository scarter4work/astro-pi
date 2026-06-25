import pytest

from gaia_depth_grade import __version__
from gaia_depth_grade.cli import main


def test_version_flag_prints_bare_version(capsys):
    # The PJSR bootstrap compares this output verbatim to the pinned
    # SIDECAR_VERSION, so it must be exactly the bare version, nothing else.
    with pytest.raises(SystemExit) as exc:
        main(["--version"])
    assert exc.value.code == 0
    assert capsys.readouterr().out.strip() == __version__
