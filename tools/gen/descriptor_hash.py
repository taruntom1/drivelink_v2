"""The descriptor hash: a CRC-32 over everything the definitions say.

It is computed over the canonical JSON (sorted keys, no whitespace) of the
normalized registry, diag codes and module types, so it changes exactly when a
definition changes. GET_INFO (F3) reports it, so the host can tell that the
firmware's definitions changed.
"""

from __future__ import annotations

import json
import zlib


def canonical_json(defs: dict) -> str:
    payload = {"registry": defs["registry"], "diag": defs["diag"], "types": defs["types"]}
    return json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def descriptor_hash(defs: dict) -> int:
    return zlib.crc32(canonical_json(defs).encode("ascii")) & 0xFFFFFFFF
