"""anv-build: build an ESP32 sketch on this machine (the Raspberry Pi, a laptop) with arduino-cli,
and flash it: over USB the first time, over CAN after that.

    anv-build firmware/nodes/bench_esp32_arduino --chip esp32s3   # just build
    anv-build my_node --usb /dev/ttyUSB0 --chip esp32              # build + flash over USB
    anv-build my_node 17 --can can1                                # build for node 17's chip, CAN
    anv-build my_node type:wheel --can can1                        # every wheel, one by one
    anv-build --check                                              # arduino-cli + core ready?

What it does for you, so a sketch needs no hand-copied files and no libraries folder:
- regenerates schema/generated/ if a YAML changed (when anv-schema is installed);
- fills the sketch's src/schema/ with the generated files of every type it includes
  (`#include "src/schema/anv_<type>.h"`), in a work copy: your sketch folder is never touched;
- compiles with --library for anv_link, anv_proto and anv_node where they are in the repo (they
  must stay there: they compile the generated protocol code in place);
- keeps a build folder per sketch and chip, so a rebuild only recompiles what changed;
- over CAN, asks each node for its chip and builds once per chip.

The repository is found from the sketch's folder (or --repo, or $ANV_REPO).
"""

from __future__ import annotations

import argparse
import filecmp
import os
import re
import shutil
import subprocess
import sys
import zlib
from collections.abc import Sequence
from pathlib import Path
from typing import TextIO

LIBRARIES = ("anv_link", "anv_proto", "anv_node")
CORE = "esp32:esp32"
CORE_VERSION = "3.3.12"  # the version CI builds with; 3.3.0 is the oldest that works (esp_twai)
FQBN = {"esp32": "esp32:esp32:esp32", "esp32s3": "esp32:esp32:esp32s3"}
SCHEMA_INCLUDE = re.compile(r'#\s*include\s*"src/schema/anv_(\w+)\.h"')
COMMON_HEADERS = ("anv_schema.h", "anv_protocol.h")


class BuildError(Exception):
    pass


def cache_dir() -> Path:
    """Work copies and build folders, one per sketch and chip (kept: rebuilds are incremental)."""
    return Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "anveshak" / "build"


# --- finding things ---------------------------------------------------------------------------


def arduino_cli() -> str:
    """Path of arduino-cli: $ANV_ARDUINO_CLI, then PATH, then ~/.local/bin (where setup puts it)."""
    env = os.environ.get("ANV_ARDUINO_CLI")
    if env:
        return env
    found = shutil.which("arduino-cli")
    if found:
        return found
    local = Path.home() / ".local" / "bin" / "arduino-cli"
    if local.is_file():
        return str(local)
    raise BuildError(
        "arduino-cli not found: run host/tools/pi/setup.sh once (or set ANV_ARDUINO_CLI)"
    )


def find_repo(start: Path, explicit: str | None = None) -> Path:
    """The repository root: the first folder at or above `start` holding firmware/lib/anv_node."""
    if explicit or os.environ.get("ANV_REPO"):
        repo = Path(explicit or os.environ["ANV_REPO"]).expanduser().resolve()
        if not (repo / "firmware" / "lib" / "anv_node").is_dir():
            raise BuildError(
                f"{repo} is not the Anveshak-CAN repository (no firmware/lib/anv_node)"
            )
        return repo
    for d in [start.resolve(), *start.resolve().parents]:
        if (d / "firmware" / "lib" / "anv_node").is_dir() and (
            d / "schema" / "generated" / "c"
        ).is_dir():
            return d
    raise BuildError(f"can't find the repository above {start}: pass --repo or set ANV_REPO")


def sketch_ino(sketch: Path) -> Path:
    """The sketch's main .ino (named like its folder, as Arduino requires)."""
    if sketch.is_file() and sketch.suffix == ".ino":
        sketch = sketch.parent
    ino = sketch / f"{sketch.name}.ino"
    if not ino.is_file():
        raise BuildError(f"{sketch} is not a sketch: expected {ino.name} inside it")
    return ino


def schema_types(sketch: Path) -> list[str]:
    """Node types the sketch includes as src/schema/anv_<type>.h."""
    types: set[str] = set()
    for f in sketch.rglob("*"):
        if f.suffix in (".ino", ".cpp", ".c", ".h", ".hpp") and "src/schema" not in f.as_posix():
            types.update(
                m
                for m in SCHEMA_INCLUDE.findall(f.read_text(errors="replace"))
                if f"anv_{m}.h" not in COMMON_HEADERS
            )
    return sorted(types)


# --- the steps --------------------------------------------------------------------------------


def regenerate(repo: Path, out: TextIO) -> None:
    """Run the schema generator if it's installed and the generated code is out of date."""
    try:
        from anv_schema import cli as schema_cli  # type: ignore[import-not-found]
    except ImportError:
        print("note: anv-schema not installed; using schema/generated/ as it is", file=out)
        return
    args = ["--dir", str(repo / "schema"), "gen", "--check"]
    if schema_cli.main(args) != 0:
        print("schema changed: regenerating schema/generated/", file=out)
        if schema_cli.main(args[:-1]) != 0:
            raise BuildError("anv-schema gen failed: fix the YAML (anv-schema check shows why)")


def _sync(src: Path, dst: Path) -> None:
    """Copy src into dst, writing only files that differ (keeps mtimes, so builds stay incremental)
    and removing what src no longer has."""
    dst.mkdir(parents=True, exist_ok=True)
    keep = set()
    for f in src.rglob("*"):
        rel = f.relative_to(src)
        if (
            rel.parts[0] == "build"
            or rel.parts[:2] == ("src", "schema")
            or "__pycache__" in rel.parts
        ):
            continue  # build output, and the generated files prepare() writes fresh
        keep.add(rel)
        t = dst / rel
        if f.is_dir():
            t.mkdir(parents=True, exist_ok=True)
        elif not t.is_file() or not filecmp.cmp(f, t, shallow=False):
            t.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, t)
    for t in sorted(dst.rglob("*"), reverse=True):
        rel = t.relative_to(dst)
        if rel.parts[:2] == ("src", "schema") or rel in keep:
            continue
        if t.is_dir():
            if not any(t.iterdir()):
                t.rmdir()
        else:
            t.unlink()


def prepare(sketch: Path, repo: Path, chip: str) -> tuple[Path, Path]:
    """Work copy of the sketch with src/schema/ filled; returns (work sketch, build folder)."""
    types = schema_types(sketch)
    if not types:
        raise BuildError(
            f'{sketch.name} includes no node type: add #include "src/schema/anv_<type>.h"'
        )
    gen = repo / "schema" / "generated" / "c"
    missing = [t for t in types if not (gen / f"anv_{t}.h").is_file()]
    if missing:
        raise BuildError(
            f"no generated code for type(s) {', '.join(missing)}: is the YAML in schema/types/ "
            "and was `anv-schema gen` run?"
        )
    base = cache_dir() / f"{sketch.name}-{chip}"
    work = base / sketch.name  # arduino-cli wants the folder named like the .ino
    _sync(sketch, work)
    schema_dir = work / "src" / "schema"
    shutil.rmtree(schema_dir, ignore_errors=True)
    schema_dir.mkdir(parents=True)
    for name in [*COMMON_HEADERS, *(f"anv_{t}.{ext}" for t in types for ext in ("h", "c"))]:
        shutil.copy2(gen / name, schema_dir / name)
    return work, base / "build"


def fqbn_for(chip: str, override: str | None = None) -> str:
    return override or os.environ.get(f"ANV_FQBN_{chip.upper()}") or FQBN[chip]


def compile_sketch(
    sketch: Path,
    repo: Path,
    chip: str,
    fqbn: str,
    out: TextIO,
    usb_port: str | None = None,
    verbose: bool = False,
) -> Path:
    """Build (and with usb_port, upload); returns the .bin."""
    work, build = prepare(sketch, repo, chip)
    cmd = [arduino_cli(), "compile", "--fqbn", fqbn, "--build-path", str(build)]
    for lib in LIBRARIES:
        cmd += ["--library", str(repo / "firmware" / "lib" / lib)]
    if usb_port:
        cmd += ["--upload", "--port", usb_port]
    if verbose:
        cmd.append("--verbose")
    cmd.append(str(work))
    first = "" if build.is_dir() else " - the first build takes a few minutes"
    print(
        f"building {sketch.name} for {chip} ({fqbn})"
        f"{' and uploading to ' + usb_port if usb_port else ''}{first}",
        file=out,
        flush=True,
    )
    tail: list[str] = []
    with subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace"
    ) as proc:
        assert proc.stdout is not None
        for line in proc.stdout:
            line = line.rstrip()
            tail = (tail + [line])[-60:]
            if (
                verbose
                or usb_port
                or "error" in line.lower()
                or line.startswith(("Sketch uses", "Global variables"))
            ):
                print(f"  {line}", file=out, flush=True)
        rc = proc.wait()
    if rc != 0:
        if not any("error" in ln.lower() for ln in tail):
            print("\n".join(f"  {ln}" for ln in tail[-20:]), file=out)
        raise BuildError(f"arduino-cli failed (exit {rc})" + _hint(tail))
    image = build / f"{sketch.name}.ino.bin"
    if not image.is_file():
        raise BuildError(f"build finished but there is no {image.name} in {build}")
    return image


def _hint(lines: Sequence[str]) -> str:
    text = "\n".join(lines)
    if ("Platform '" in text and "not found" in text) or (
        "esp32:esp32" in text and "not installed" in text
    ):
        return (
            ": the esp32 core isn't installed - run host/tools/pi/setup.sh "
            f"(or arduino-cli core install {CORE}@{CORE_VERSION})"
        )
    if "ld-linux-aarch64" in text or ("no such file or directory" in text and "xtensa" in text):
        return (
            ": the 64-bit compiler can't run on this 32-bit OS - use 64-bit Raspberry Pi OS "
            "(see host/tools/pi/README.md)"
        )
    if "multiple definition of" in text and "anv_" in text:
        return ": a generated .c is compiled twice - don't add anv_protocol.c to the sketch"
    if "Permission denied" in text and "/dev/tty" in text:
        return ": no access to the USB port - add yourself to the dialout group and log in again"
    return ""


def check(out: TextIO) -> int:
    """Is everything there to build?"""
    try:
        cli = arduino_cli()
    except BuildError as e:
        print(e, file=out)
        return 1
    version = subprocess.run([cli, "version"], capture_output=True, text=True).stdout.strip()
    cores = subprocess.run([cli, "core", "list"], capture_output=True, text=True).stdout
    line = next((ln for ln in cores.splitlines() if ln.startswith(CORE + " ")), "")
    print(f"arduino-cli: {cli} ({version or 'version unknown'})", file=out)
    if not line:
        print("esp32 core: NOT installed - run host/tools/pi/setup.sh", file=out)
        return 1
    installed = line.split()[1]
    print(
        f"esp32 core: {installed}"
        + ("" if installed >= "3.3.0" else "  (too old: 3.3.0 or newer needed)"),
        file=out,
    )
    return 0 if installed >= "3.3.0" else 1


# --- over CAN ---------------------------------------------------------------------------------


def build_and_flash_can(
    sketch: Path, repo: Path, a: argparse.Namespace, out: TextIO, open_link=None
) -> int:
    from . import Host, HostError, load_schema
    from . import flash_cli as fl

    # The repo's freshly generated schema, so a new node type works without reinstalling anv_can.
    repo_schema = repo / "schema" / "generated" / "schema.json"
    schema = load_schema(
        a.schema
        or os.environ.get("ANV_SCHEMA_JSON")
        or (str(repo_schema) if repo_schema.is_file() else None)
    )
    host_link = fl._open(open_link, a.can)
    transfer_link = fl._open(open_link, a.can)
    host = Host(host_link, schema, host_id=a.tool_id, observer=True)  # never leads or arms
    try:
        host.start()
        if a.target == "all" or a.target.startswith("type:") or "," not in a.target:
            import time

            time.sleep(a.wait)
        targets = fl._resolve_targets(host, a.target)
        if not targets:
            print(f"anv-build: no nodes match {a.target!r} on {a.can}", file=out)
            return 1
        # Tell each node which type the image is for, so a board running another type refuses it
        # (anv-flash sends the node's own type, which always matches).
        types = schema_types(sketch)
        sketch_type_id = schema.type(types[0]).type_id if len(types) == 1 else None
        by_chip: dict[str, list[int]] = {}
        for node in targets:
            if not host.wait_for_node(node, int(a.wait * 1000)):
                print(f"node {node}: not heard on {a.can}, skipping", file=out)
                continue
            chip = a.chip or fl.CHIP_NAMES.get(fl._chip_of(host, node), "unknown")
            if chip not in FQBN:
                print(
                    f"node {node}: chip {chip!r} unknown (pass --chip esp32 or esp32s3)", file=out
                )
                continue
            by_chip.setdefault(chip, []).append(node)
        flashed = 0
        total = sum(len(v) for v in by_chip.values())
        for chip, nodes in sorted(by_chip.items()):
            image = compile_sketch(
                sketch, repo, chip, fqbn_for(chip, a.fqbn), out, verbose=a.verbose
            ).read_bytes()
            crc = zlib.crc32(image) & 0xFFFFFFFF
            print(
                f"flashing {len(image)} bytes (crc32 {crc:08x}) over {a.can} "
                f"to {chip} node(s) {nodes}",
                file=out,
            )
            fa = argparse.Namespace(
                wait=a.wait,
                type_id=sketch_type_id,
                chip=None,
                tool_id=a.tool_id,
                timeout_ms=a.timeout_ms,
                force=a.force,
                chunk=fl.DEFAULT_CHUNK,
            )
            for node in nodes:
                if fl._flash_one(host, schema, transfer_link, fa, node, image, crc, out):
                    flashed += 1
        print(f"{flashed}/{len(targets)} node(s) flashed", file=out)
        return 0 if flashed == len(targets) and total == len(targets) else 1
    except HostError as e:
        print(f"anv-build: {e}", file=sys.stderr)
        return 1
    finally:
        host.stop()


# --- command line -----------------------------------------------------------------------------


def _parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="anv-build", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("sketch", nargs="?", help="the sketch folder (or its .ino)")
    ap.add_argument(
        "target", nargs="?", help="with --can: node ID, 'id,id', 'type:<name>' or 'all'"
    )
    ap.add_argument(
        "--chip",
        choices=sorted(FQBN),
        help="build for this chip (default with --can: each node's own)",
    )
    ap.add_argument(
        "--fqbn", help="full board name, for board options (e.g. esp32:esp32:esp32s3:CDCOnBoot=cdc)"
    )
    ap.add_argument(
        "--usb",
        metavar="PORT",
        help="upload over USB to this port (e.g. /dev/ttyUSB0, /dev/ttyACM0)",
    )
    ap.add_argument("--can", metavar="IFACE", help="flash over CAN on this interface (e.g. can1)")
    ap.add_argument("-o", "--output", help="also copy the .bin here (file or folder)")
    ap.add_argument("--repo", help="repository root (default: found from the sketch, or $ANV_REPO)")
    ap.add_argument("--no-gen", action="store_true", help="don't regenerate schema/generated/")
    ap.add_argument(
        "--check",
        action="store_true",
        help="only check that arduino-cli and the esp32 core are ready",
    )
    ap.add_argument("-v", "--verbose", action="store_true", help="show the whole compiler output")
    ap.add_argument(
        "--schema", help="(with --can) schema.json; default $ANV_SCHEMA_JSON, else the repo's"
    )
    ap.add_argument(
        "--tool-id",
        type=lambda x: int(x, 0),
        default=int(os.environ.get("ANV_HOST_ID", "250"), 0),
        help="(with --can) this tool's host ID (default 250)",
    )
    ap.add_argument(
        "--wait", type=float, default=2.0, help="(with --can) seconds to wait for nodes"
    )
    ap.add_argument("--timeout-ms", type=int, default=2000, help="(with --can) per-message timeout")
    ap.add_argument(
        "--force", action="store_true", help="(with --can) clear a half-finished update first"
    )
    return ap


def main(argv: list[str] | None = None, open_link=None, out: TextIO | None = None) -> int:
    ap = _parser()
    a = ap.parse_args(argv)
    out = out or sys.stdout
    if a.check:
        return check(out)
    if not a.sketch:
        ap.error("give a sketch folder (or --check)")
    if a.usb and a.can:
        ap.error("--usb or --can, not both")
    if a.can and not a.target:
        ap.error("--can needs a target: node ID, 'type:<name>' or 'all'")
    if a.target and not a.can:
        ap.error(f"unexpected {a.target!r}: a target only goes with --can")
    if not a.can and not a.chip and not a.fqbn:
        ap.error("--chip esp32 or --chip esp32s3 (only --can finds the chip by itself)")
    try:
        sketch = sketch_ino(Path(a.sketch).expanduser()).parent.resolve()
        repo = find_repo(sketch, a.repo)
        if not a.no_gen:
            regenerate(repo, out)
        if a.can:
            return build_and_flash_can(sketch, repo, a, out, open_link)
        chip = a.chip or next(c for c, f in FQBN.items() if a.fqbn.split(":")[2] == f.split(":")[2])
        image = compile_sketch(
            sketch, repo, chip, fqbn_for(chip, a.fqbn), out, usb_port=a.usb, verbose=a.verbose
        )
        if a.output:
            dest = Path(a.output)
            if dest.is_dir():
                dest = dest / f"{sketch.name}-{chip}.bin"
            shutil.copy2(image, dest)
            image = dest
        print(
            f"{'uploaded; ' if a.usb else ''}image: {image} ({image.stat().st_size} bytes)",
            file=out,
        )
        if not a.usb:
            print(f"to flash it over CAN: anv-build {a.sketch} <node> --can can1", file=out)
        return 0
    except (BuildError, StopIteration) as e:
        print(f"anv-build: {e or 'unknown chip in --fqbn'}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
