from pathlib import Path
import shutil
import subprocess
import tempfile

from scikit_build_core.build import *
from scikit_build_core.build import build_wheel as _build_wheel


def build_wheel(wheel_directory, config_settings=None, metadata_directory=None):
    destination = Path(wheel_directory).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lokiverse-wheel-") as temporary:
        root = Path(temporary)
        original = root / "original"
        repaired = root / "repaired"
        original.mkdir()
        repaired.mkdir()
        filename = _build_wheel(str(original), config_settings, metadata_directory)
        subprocess.run(
            ["repairwheel", str(original / filename), "-o", str(repaired)],
            check=True,
        )
        wheels = list(repaired.glob("*.whl"))
        if len(wheels) != 1:
            raise RuntimeError("wheel repair must produce exactly one wheel")
        wheel = wheels[0]
        shutil.copyfile(wheel, destination / wheel.name)
        return wheel.name
