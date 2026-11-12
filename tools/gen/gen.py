#!/usr/bin/env python3
"""Generate the protocol's C++ headers and docs from protocol/defs/.

Reads protocol/defs/registry.yaml, diag_codes.yaml and one YAML file per module
type. Writes:

  protocol/include/drivelink/protocol/gen/registry.hpp    MSG_TYPEs, channel ranges, descriptor hash
  protocol/include/drivelink/protocol/gen/diag_codes.hpp  the diag code table
  protocol/include/drivelink/protocol/gen/types.hpp       the type table
  protocol/include/drivelink/protocol/gen/type_<type>.hpp one descriptor table per module type
  docs/protocol/registry.md                               the tables between marker comments
  docs/protocol/diag-codes.md                             the code table between marker comments
  docs/protocol/types/<type>.md                           one page per module type

Usage:
  python tools/gen/gen.py           regenerate
  python tools/gen/gen.py --check   exit 1 if any generated file is stale (CI)

Internal options, for the generator's own tests: --defs DIR reads the YAML from
DIR, and --out DIR writes under DIR (at the same paths) instead of the repo.

Needs PyYAML.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import descriptor_hash
import emit_cpp
import emit_docs
import model

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DEFS = ROOT / "protocol" / "defs"

# Directories whose generated files are all owned by the generator: a file
# there that it no longer produces is stale (a removed type).
OWNED = (
    (emit_cpp.GEN_DIR, "type_*.hpp"),
    (emit_docs.TYPES_DOC_DIR, "*.md"),
)


def normalize(text: str) -> str:
    return text.replace("\r\n", "\n")


def read_text(path: Path) -> str:
    return normalize(path.read_text(encoding="utf-8")) if path.exists() else ""


def generate(defs_dir: Path, out_dir: Path) -> dict[str, str]:
    """Return {path relative to out_dir (with '/'): text} for every generated file."""
    defs = model.load_defs(defs_dir)
    hash_value = descriptor_hash.descriptor_hash(defs)
    gen = emit_cpp.GEN_DIR

    outputs: dict[str, str] = {
        f"{gen}/registry.hpp": emit_cpp.gen_registry(defs, hash_value),
        f"{gen}/diag_codes.hpp": emit_cpp.gen_diag(defs),
        f"{gen}/types.hpp": emit_cpp.gen_types(defs),
    }
    for t in defs["types"]:
        outputs[f"{gen}/{emit_cpp.type_header_name(t)}"] = emit_cpp.gen_type(t)
        outputs[emit_docs.type_doc_path(t)] = emit_docs.type_doc(t)

    outputs[emit_docs.REGISTRY_DOC] = emit_docs.splice(
        read_text(out_dir / emit_docs.REGISTRY_DOC),
        emit_docs.REGISTRY_TEMPLATE,
        emit_docs.REGISTRY_BEGIN,
        emit_docs.REGISTRY_END,
        emit_docs.registry_block(defs, hash_value),
        emit_docs.REGISTRY_DOC,
    )
    outputs[emit_docs.DIAG_DOC] = emit_docs.splice(
        read_text(out_dir / emit_docs.DIAG_DOC),
        emit_docs.DIAG_TEMPLATE,
        emit_docs.DIAG_BEGIN,
        emit_docs.DIAG_END,
        emit_docs.diag_block(defs),
        emit_docs.DIAG_DOC,
    )
    return outputs


def run(defs_dir: Path, out_dir: Path, check: bool) -> int:
    try:
        outputs = generate(defs_dir, out_dir)
    except model.SpecError as e:
        print(f"gen: {e}", file=sys.stderr)
        return 2

    stale: list[str] = []
    for rel, text in outputs.items():
        path = out_dir / rel
        current = read_text(path) if path.exists() else None
        if current == text:
            continue
        stale.append(rel)
        if not check:
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("w", encoding="utf-8", newline="\n") as f:
                f.write(text)

    orphans: list[str] = []
    for directory, pattern in OWNED:
        d = out_dir / directory
        if d.is_dir():
            for path in sorted(d.glob(pattern)):
                rel = path.relative_to(out_dir).as_posix()
                if rel not in outputs:
                    orphans.append(rel)
                    if not check:
                        path.unlink()

    for rel in stale:
        print(f"{'stale' if check else 'updated'}: {rel}")
    for rel in orphans:
        print(f"{'orphan' if check else 'removed'}: {rel}")
    if check and (stale or orphans):
        print("Run `python tools/gen/gen.py` and commit the result.", file=sys.stderr)
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="fail if a generated file is stale; write nothing")
    parser.add_argument("--defs", type=Path, default=DEFAULT_DEFS, help=argparse.SUPPRESS)
    parser.add_argument("--out", type=Path, default=ROOT, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    return run(args.defs, args.out, args.check)


if __name__ == "__main__":
    sys.exit(main())
