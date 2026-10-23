import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from image_format import package


def main():
    p = argparse.ArgumentParser(description="Demonstrate trial confirmation and rollback using temporary Flash files.")
    p.add_argument("--binary", default="build/fwctl")
    args = p.parse_args()
    binary = str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory(prefix="fw-update-") as folder:
        root = Path(folder)
        flash = root / "device.flash"
        images = {}
        for version in (1, 2, 3):
            images[version] = root / f"v{version}.img"
            images[version].write_bytes(package(bytes([version]) * 67, version))

        def run(label, *argv):
            output = subprocess.check_output([binary, *map(str, argv)], text=True)
            print(label)
            print(json.dumps(json.loads(output), indent=2))

        run("Initialize Flash", "init", flash)
        run("Provision confirmed version 1", "provision", flash, images[1])
        run("Stage version 2", "stage", flash, images[2])
        run("Select version 2 for one trial", "boot", flash)
        run("No confirmation: next boot rolls back", "boot", flash)
        run("Stage version 2 again", "stage", flash, images[2])
        run("Select version 2 for trial", "boot", flash)
        run("Application confirms version 2", "confirm", flash, 1, 2)
        run("Version 2 is now stable", "boot", flash)
        run("Interrupted version 3 write", "status", flash)
        cut = subprocess.run([binary, "stage", str(flash), str(images[3]),
                              "--cut-at", "18", "--tear", "2"], capture_output=True, text=True)
        if cut.returncode != 86:
            raise RuntimeError(f"expected simulated power loss: {cut}")
        print(json.dumps({"simulated_power_loss_exit": cut.returncode}))
        run("Restart still selects confirmed version 2", "boot", flash)


if __name__ == "__main__":
    main()
