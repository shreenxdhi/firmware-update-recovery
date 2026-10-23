import argparse
import json
from pathlib import Path
from image_format import package


def main():
    p = argparse.ArgumentParser(description="Create a firmware package with explicit header and CRC fields.")
    p.add_argument("--version", type=int, required=True)
    p.add_argument("--output", type=Path, required=True)
    group = p.add_mutually_exclusive_group()
    group.add_argument("--payload", type=Path)
    group.add_argument("--size", type=int, default=67)
    a = p.parse_args()
    try:
        if not 0 < a.version <= 0xFFFFFFFF:
            raise ValueError("version must be 1..4294967295")
        if a.payload:
            if a.payload.stat().st_size > 4064:
                raise ValueError("payload exceeds slot capacity")
            payload = a.payload.read_bytes()
        else:
            if not 0 < a.size <= 4064:
                raise ValueError("size must be 1..4064")
            payload = bytes((a.version + i * 17) & 0xFF for i in range(a.size))
        data = package(payload, a.version)
        with a.output.open("xb") as f:
            f.write(data)
    except (OSError, ValueError) as error:
        p.error(str(error))
    print(json.dumps({"result": "ok", "version": a.version, "payload_bytes": len(payload)}))


if __name__ == "__main__":
    main()
