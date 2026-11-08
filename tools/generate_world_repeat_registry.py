#!/usr/bin/env python3
"""Generate world-program ownership metadata from legally supplied native RPKs.

Usage: python tools/generate_world_repeat_registry.py global.rpk vehicleglobal.rpk
Semantic source-family categories identify potential large surface materials;
they do not establish that a particular mesh is flat. Unknown owners are denied.
No texture IDs, shader instructions, game files or instance-name whitelist are emitted.
"""
import argparse
import hashlib
from pathlib import Path
import re
import struct


class Reader:
    def __init__(self, data, offset=0):
        self.data, self.offset = data, offset

    def read(self, fmt):
        result = struct.unpack_from("<" + fmt, self.data, self.offset)
        self.offset += struct.calcsize("<" + fmt)
        return result[0] if len(result) == 1 else result

    def take(self, size):
        start = self.offset
        self.offset += size
        if self.offset > len(self.data):
            raise ValueError("Truncated RPK")
        return self.data[start:self.offset]

    def header(self):
        if self.take(8) != b"EVOSLITL":
            raise ValueError("Unexpected RPK header")

    def string(self):
        return self.take(self.read("I")).decode("utf-8")


def resources(data):
    reader = Reader(data)
    reader.header()
    if reader.string() != "Resource PacK file":
        raise ValueError("Not an RPK resource package")
    _, index = reader.read("II")
    reader.offset = index
    reader.header()
    uid, version = reader.read("QI")
    if uid >> 48 != 1 or reader.read("I") != 0x11111111:
        raise ValueError("Unexpected RPK index")
    count, _, _, name_size, source_size = reader.read("IIIII")
    if version >= 0x40002:
        reader.read("II")
    reader.read("Q")
    names, sources = reader.take(name_size), reader.take(source_size)

    def strings(pool):
        result = []
        for _ in range(reader.read("H")):
            offset = reader.read("I")
            result.append(pool[offset:pool.index(0, offset)].decode("utf-8"))
        return result

    result = []
    for _ in range(count):
        uid, size, offset = reader.read("QII")
        deps = [reader.read("Q") for _ in range(reader.read("H"))]
        result.append(dict(uid=uid, size=size, offset=offset, dependencies=deps,
                           names=strings(names), sources=strings(sources), package=data))
    return result


WORLD_MATERIAL_ROOT = "assets/noodle/materials/atg1/"
# Exclusions precede positive categories: treewall and foliagegroundblend are
# vegetation. The native families use both vegetation and vegitation spellings.
VEGETATION_FAMILY = re.compile(
    r"foliage|grass|leaves|needles|tree|trunk|bark|branch|bush|vegetation|vegitation|billboard|plantation")
COMPLEX_OBJECT_FAMILY = re.compile(
    r"^(?:clothsim|flag|circlingbirds|rotatingmesh|rockingboat)|(?:^|_)deform(?:_|$)")
EFFECT_FAMILY = re.compile(
    r"alpha|cutout|glass|wind|shadow|emissive|light|preview|debug|sky|animated")
# Authored symbols and overlays must not be randomized with physical surface grain.
AUTHORED_GRAPHIC_FAMILY = re.compile(
    r"decal|marking|paintmark|^roadline|^roadmark|(?:^|_)(?:sign|mural|logo|graphic|design)(?:_|$)")
SURFACE_FAMILY = re.compile(
    r"^(?:road|ground|concrete|pavement|pavment|brick|roof|tiles|tarmac)|wall(?:_|$)")


def is_world_surface_source(source):
    """Positive native surface-family provenance, not geometric flatness.

    The source identifies an authoring family, not a renamed material instance
    or texture. Cabin families belong to a separate cohort, never this set.
    """
    if not source.startswith(WORLD_MATERIAL_ROOT):
        return False
    family = source[len(WORLD_MATERIAL_ROOT):]
    if "/" in family or "\\" in family or not family.endswith(".material"):
        return False
    family = family[:-len(".material")].lower()
    return bool(family and not VEGETATION_FAMILY.search(family) and
                not COMPLEX_OBJECT_FAMILY.search(family) and
                not EFFECT_FAMILY.search(family) and
                not AUTHORED_GRAPHIC_FAMILY.search(family) and SURFACE_FAMILY.search(family))


def pixel_program_owners(entries):
    """Collect all owners before filtering, including unknown provenance.

    Different shader resource IDs can contain the same native program.
    """
    by_id = {entry["uid"]: entry for entry in entries}
    owners = {}
    for material in entries:
        if material["uid"] >> 48 != 6:
            continue
        programs = set()
        for dep in set(material["dependencies"]):
            shader = by_id.get(dep)
            if not shader or shader["uid"] >> 48 != 29 or shader["names"] != ["PixelShader"]:
                continue
            blob = shader["package"][shader["offset"]:shader["offset"] + shader["size"]]
            for marker in re.finditer(b"OrbShdr", blob):
                if marker.start() + 24 > len(blob):
                    raise ValueError("Truncated shader metadata")
                program = struct.unpack_from("<Q", blob, marker.start() + 16)[0]
                programs.add(program)
        for program in programs:
            owners.setdefault(program, []).append(material)
    return owners


def classify(entries, source_eligible=is_world_surface_source):
    """Require every source of every known owner to qualify for one cohort.

    Separate cohorts can reuse this closure with another source predicate;
    default world classification never includes vehicle or interior materials.
    """
    return sorted(program for program, owners in pixel_program_owners(entries).items()
                  if all(owner["sources"] and all(source_eligible(source)
                                                 for source in owner["sources"])
                         for owner in owners))


def render(programs):
    lines = ["// Generated by tools/generate_world_repeat_registry.py; do not edit by hand.",
             "// Positive native world-surface families; all known owners must qualify.",
             "// Surface-family scope only, not a geometric flatness proof; interiors excluded.",
             "// No texture IDs or shader instructions."]
    for start in range(0, len(programs), 6):
        lines.append(" ".join(f"0x{value:016x}ull," for value in programs[start:start + 6]))
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packages", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] /
                        "gpu/shadps4/shader_recompiler/backend/spirv/world_repeat_programs.inc")
    args = parser.parse_args()
    entries = []
    for path in args.packages:
        blob = path.read_bytes()
        print(path.name, hashlib.sha256(blob).hexdigest())
        entries.extend(resources(blob))
    programs = classify(entries)
    args.output.write_text(render(programs), encoding="utf-8", newline="\n")
    print(f"Wrote {len(programs)} world-surface pixel programs to {args.output}")


if __name__ == "__main__":
    main()
