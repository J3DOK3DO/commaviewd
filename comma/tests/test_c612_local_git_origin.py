"""C6-12 origin checks for trusted local Git checkout, no working tree writes."""
from pathlib import Path
import shlex
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[2] / "comma" / "scripts"
SCRIPTS = ("apply_onroad_ui_export_patch.sh", "verify_onroad_ui_export_patch.sh")


def classify(script, remote, *, flavor="openpilot", root="/data/openpilot",
             state_root="/data/openpilot", state_remote="/data/openpilot",
             state_head="7b4db9a2", live_head="7b4db9a2"):
    text = (ROOT / script).read_text()
    start = text.index("remote_flavor() {\n")
    end = text.index("\ndetect_flavor() {", start)
    fn = text[start:end]
    q = shlex.quote
    shell = "\n".join([
        "OP_ROOT=" + q(root),
        "state_value() {",
        "  case \"$1\" in",
        "    ONROAD_UI_EXPORT_FLAVOR) printf '%s\\n' " + q(flavor) + " ;;",
        "    ONROAD_UI_EXPORT_OP_ROOT) printf '%s\\n' " + q(state_root) + " ;;",
        "    ONROAD_UI_EXPORT_UPSTREAM_REMOTE) printf '%s\\n' " + q(state_remote) + " ;;",
        "    ONROAD_UI_EXPORT_UPSTREAM_HEAD) printf '%s\\n' " + q(state_head) + " ;;",
        "    *) return 1 ;;",
        "  esac",
        "}",
        "upstream_head() { printf '%s\\n' " + q(live_head) + "; }",
        fn,
        "remote_flavor " + q(remote),
    ])
    result = subprocess.run(["bash", "-c", shell], capture_output=True,
                            text=True, check=False, timeout=5)
    return result.returncode, result.stdout.strip()


@pytest.mark.parametrize("script", SCRIPTS)
@pytest.mark.parametrize("remote,expected,options", [
    ("/data/openpilot", (0, "openpilot"), {}),
    ("/data/openpilot", (1, ""), {"state_head": "stale"}),
    ("/data/openpilot", (1, ""), {"state_remote": "/tmp/other"}),
    ("/data/openpilot", (1, ""), {"state_root": "/tmp/other"}),
    ("/data/openpilot", (1, ""), {"flavor": "starpilot"}),
    ("/data/openpilot", (1, ""), {"live_head": ""}),
    ("/data/openpilot", (1, ""), {"root": "/tmp/openpilot"}),
    ("/data/openpilot2", (1, ""), {}),
    ("https://github.com/J3DOK3DO/openpilot.git", (0, "openpilot"), {}),
    ("git@github.com:firestar5683/StarPilot.git", (0, "starpilot"), {}),
    ("https://notgithub.com/j3dok3do/openpilot", (1, ""), {}),
    ("git@evilgithub.com:j3dok3do/openpilot", (1, ""), {}),
])
def test_local_origin_and_supported_remotes(script, remote, expected, options):
    assert classify(script, remote, **options) == expected
