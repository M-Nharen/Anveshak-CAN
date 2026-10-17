"""The generated C must match the Python codec byte for byte, and must build cleanly."""
from __future__ import annotations

import random
import shutil
import subprocess

import pytest
from conftest import SCHEMA_DIR, all_messages, random_raw

from anv_schema import encode
from anv_schema.cli import generated_files, main
from anv_schema.gen_c import prefix

GEN_C = SCHEMA_DIR / "generated" / "c"
STRICT = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wsign-conversion",
          "-Wshadow", "-Wdouble-promotion", "-pedantic"]

needs_gcc = pytest.mark.skipif(shutil.which("gcc") is None, reason="no gcc")


def test_generated_files_are_current(schema):
    """CI's guard: change a YAML file, forget `anv-schema gen`, and this fails."""
    for rel, text in generated_files(schema).items():
        path = SCHEMA_DIR / "generated" / rel
        assert path.is_file(), f"{rel} missing; run anv-schema gen"
        assert path.read_text(encoding="utf-8") == text, f"{rel} stale; run anv-schema gen"
    assert main(["--dir", str(SCHEMA_DIR), "gen", "--check"]) == 0


@needs_gcc
@pytest.mark.parametrize("extra", [[], ["-DANV_SCHEMA_PHYS_DOUBLE"], ["-DANV_SCHEMA_NO_NAMES"]])
def test_generated_c_builds_strictly(tmp_path, extra):
    for src in sorted(GEN_C.glob("*.c")):
        subprocess.run(["gcc", *STRICT, *extra, f"-I{GEN_C}", "-c", str(src), "-o",
                        str(tmp_path / (src.stem + ".o"))], check=True)


@needs_gcc
def test_generated_headers_build_as_cpp(tmp_path):
    if shutil.which("g++") is None:
        pytest.skip("no g++")
    src = tmp_path / "t.cpp"
    src.write_text('#include "anv_all_types.h"\nint main() { return anv_all_types[0] ? 0 : 1; }\n')
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", f"-I{GEN_C}",
                    "-fsyntax-only", str(src)], check=True)


def _lit(f, v) -> str:
    if f.type.is_float:
        return f"{float(v).hex()}f" if v == v else "NAN"
    if f.type.is_bool:
        return "true" if v else "false"
    return f"({v + 1}LL - 1)" if v < 0 else f"{v}ULL"


def _c_vectors(schema, rng) -> tuple[str, int]:
    """A C program that packs and unpacks random vectors and compares with Python's bytes."""
    lines = ['#include <stdio.h>', '#include <string.h>', '#include "anv_all_types.h"', "",
             "static int failures = 0;",
             "#define CHECK(c, what) do { if (!(c)) { failures++; "
             'printf("FAIL %s\\n", what); } } while (0)', "", "int main(void)", "{"]
    n = 0
    for tname, msg in all_messages(schema):
        t = schema.protocol if tname == "protocol" else schema.types[tname]
        name = f"{prefix(t)}{msg.name}"
        for k in range(10):
            values = random_raw(msg, rng)
            data = encode(msg, values, raw=True)
            tag = f"{name}#{k}"
            lines.append("    {")
            lines.append(f"        {name}_t m;")
            lines.append("        memset(&m, 0, sizeof m);")
            for f in msg.fields:
                if f.count > 1:
                    for i, v in enumerate(values[f.name]):
                        lines.append(f"        m.{f.name}[{i}] = ({f.type.c_type}){_lit(f, v)};")
                else:
                    lines.append(f"        m.{f.name} = ({f.type.c_type}){_lit(f, values[f.name])};")
            exp = ", ".join(f"0x{b:02x}" for b in data) or "0"
            lines.append(f"        const uint8_t expected[8] = {{{exp}}};")
            lines.append("        uint8_t out[8];")
            lines.append(f"        uint8_t len = {name}_pack(out, &m);")
            lines.append(f'        CHECK(len == {msg.length}u, "{tag} length");')
            lines.append(f'        CHECK(memcmp(out, expected, {msg.length}u) == 0, "{tag} pack");')
            lines.append(f"        {name}_t back;")
            lines.append(f'        CHECK({name}_unpack(&back, expected, {msg.length}u), "{tag} unpack");')
            for f in msg.fields:
                idx = [f"[{i}]" for i in range(f.count)] if f.count > 1 else [""]
                for s in idx:
                    if f.type.is_float:
                        lines.append(f'        CHECK(memcmp(&back.{f.name}{s}, &m.{f.name}{s}, 4) == 0, '
                                     f'"{tag} {f.name}");')
                    else:
                        lines.append(f'        CHECK(back.{f.name}{s} == m.{f.name}{s}, "{tag} {f.name}");')
            if msg.length > 0:
                lines.append(f'        CHECK(!{name}_unpack(&back, expected, {msg.length - 1}u), '
                             f'"{tag} short frame accepted");')
            lines.append("    }")
            n += 1
    # descriptor tables agree with the schema
    for t in [schema.protocol, *schema.types.values()]:
        tn = "anv_protocol" if t.type_id == 0 else f"anv_{t.name}"
        lines.append(f'    CHECK({tn}_type.n_messages == {len(t.messages)}u, "{tn} n_messages");')
        lines.append(f'    CHECK({tn}_type.n_params == {len(t.params)}u, "{tn} n_params");')
        lines.append(f'    CHECK({tn}_type.n_events == {len(t.events)}u, "{tn} n_events");')
        lines.append(f'    CHECK({tn}_type.wire_hash == {t.wire_hash:#x}u, "{tn} hash");')
        if t.type_id:
            lines.append(f'    CHECK(anv_type_by_id({t.type_id}u) == &{tn}_type, "{tn} by id");')
    lines.append(f'    CHECK(anv_id_make(2, 16, 64, 1) == {schema.id_layout.make(priority=2, dst=16, port=64, src=1):#x}u, "id");')
    lines.append('    CHECK(anv_id_port(anv_id_make(7, 254, 1023, 253)) == 1023u, "id port");')
    lines.append('    CHECK(anv_id_dst(anv_id_make(7, 254, 1023, 253)) == 254u, "id dst");')
    lines += ['    printf("%d failures\\n", failures);', "    return failures != 0;", "}", ""]
    return "\n".join(lines), n


@needs_gcc
def test_c_matches_python_byte_for_byte(schema, tmp_path):
    program, n = _c_vectors(schema, random.Random(3))
    assert n > 100
    src = tmp_path / "vectors.c"
    src.write_text(program)
    exe = tmp_path / "vectors"
    sources = [str(p) for p in sorted(GEN_C.glob("*.c"))]
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-overflow",
                    f"-I{GEN_C}", str(src), *sources, "-o", str(exe)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout[-2000:]


@needs_gcc
def test_phys_helpers(tmp_path):
    src = tmp_path / "phys.c"
    src.write_text("""
#include <stdio.h>
#include "anv_motor_controller.h"
int main(void) {
    int bad = 0;
    bad |= anv_motor_controller_setpoint_target_from_phys(1.5f) != 150;
    bad |= anv_motor_controller_setpoint_target_from_phys(-1.505f) != -151;
    bad |= anv_motor_controller_setpoint_target_from_phys(1e9f) != 32767;      /* clamped */
    bad |= anv_motor_controller_setpoint_target_from_phys(-1e9f) != -32768;
    bad |= anv_motor_controller_feedback_current_from_phys(-3.0f) != 0;         /* unsigned */
    float v = anv_motor_controller_setpoint_target_to_phys(-250);
    bad |= v < -2.5001f || v > -2.4999f;
    printf("%d\\n", bad);
    return bad;
}
""")
    exe = tmp_path / "phys"
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{GEN_C}", str(src),
                    str(GEN_C / "anv_motor_controller.c"), "-o", str(exe)], check=True)
    assert subprocess.run([str(exe)]).returncode == 0
