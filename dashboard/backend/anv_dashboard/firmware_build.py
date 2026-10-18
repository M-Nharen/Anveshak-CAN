"""Turn an uploaded `user_app.cpp` into a firmware image for one chip, with a cache.

The bench sketch (firmware/nodes/bench_esp32_arduino) is built with the uploaded file as its
`user_app.cpp`, by `arduino-cli` and the three v2 libraries. The image is cached under a key made
from the file, the chip's FQBN and everything else that goes into the build (the rest of the
sketch, firmware/lib, the generated schema code), so uploading the same file again for the same
chip skips straight to flashing, and any change to the sources builds afresh.
"""

from __future__ import annotations

import asyncio
import hashlib
import shutil
import tempfile
from collections.abc import Callable, Iterable
from pathlib import Path

SKETCH = "bench_esp32_arduino"
LIBRARIES = ("anv_link", "anv_proto", "anv_node")
IMAGE_NAME = f"{SKETCH}.ino.bin"


class BuildError(Exception):
    pass


def _files(roots: Iterable[Path], skip: Callable[[Path], bool]) -> list[Path]:
    out = []
    for root in roots:
        if root.is_file():
            out.append(root)
        elif root.is_dir():
            out.extend(p for p in sorted(root.rglob("*")) if p.is_file() and not skip(p))
    return out


def source_hash(repo: Path) -> str:
    """Everything that goes into the build except user_app.cpp."""
    sketch = repo / "firmware" / "nodes" / SKETCH

    def skip(p: Path) -> bool:
        rel = p.relative_to(repo).parts
        return "build" in rel or p.name == "user_app.cpp" or p.suffix in (".bin", ".elf", ".map")

    roots = [sketch, *(repo / "firmware" / "lib" / lib for lib in LIBRARIES), repo / "schema" / "generated" / "c"]
    h = hashlib.sha256()
    for f in _files(roots, skip):
        h.update(str(f.relative_to(repo)).encode())
        h.update(f.read_bytes())
    return h.hexdigest()


def cache_key(user_app: bytes, fqbn: str, sources: str) -> str:
    h = hashlib.sha256()
    for part in (user_app, fqbn.encode(), sources.encode()):
        h.update(len(part).to_bytes(4, "little"))
        h.update(part)
    return h.hexdigest()[:24]


def cached_image(cache_dir: Path, key: str) -> Path | None:
    image = cache_dir / key / IMAGE_NAME
    return image if image.is_file() else None


async def build(
    repo: Path,
    user_app: bytes,
    fqbn: str,
    cache_dir: Path,
    key: str,
    arduino_cli: str,
    on_line: Callable[[str], None],
    on_start: Callable[[asyncio.subprocess.Process], None] = lambda proc: None,
) -> Path:
    """Compile the sketch with `user_app` and store the image in the cache; returns its path."""
    sketch = repo / "firmware" / "nodes" / SKETCH
    tail: list[str] = []
    with tempfile.TemporaryDirectory(prefix="anv-build-") as tmp:
        work = Path(tmp) / SKETCH  # arduino-cli wants the folder named like the .ino
        shutil.copytree(sketch, work, ignore=shutil.ignore_patterns("build"))
        (work / "user_app.cpp").write_bytes(user_app)
        out_dir = Path(tmp) / "out"
        cmd = [arduino_cli, "compile", "--fqbn", fqbn]
        for lib in LIBRARIES:
            cmd += ["--library", str(repo / "firmware" / "lib" / lib)]
        cmd += ["--output-dir", str(out_dir), str(work)]
        try:
            proc = await asyncio.create_subprocess_exec(
                *cmd, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT
            )
        except FileNotFoundError as e:
            raise BuildError(f"arduino-cli not found ({arduino_cli}); install it on the Pi first") from e
        on_start(proc)
        assert proc.stdout is not None
        async for raw in proc.stdout:
            line = raw.decode(errors="replace").rstrip()
            if line:
                tail = (tail + [line])[-40:]
                on_line(line)
        rc = await proc.wait()
        if rc != 0:
            errors = [ln for ln in tail if "error" in ln.lower()] or tail
            raise BuildError("build failed:\n" + "\n".join(errors[-8:]))
        image = out_dir / IMAGE_NAME
        if not image.is_file():
            raise BuildError("build finished but produced no .ino.bin")
        dest = cache_dir / key
        dest.mkdir(parents=True, exist_ok=True)
        shutil.copy2(image, dest / IMAGE_NAME)
        (dest / "user_app.cpp").write_bytes(user_app)  # what this image was built from
        return dest / IMAGE_NAME
