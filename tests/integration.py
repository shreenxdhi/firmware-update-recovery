import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from image_format import package, parse, SLOT_OFFSETS, SLOT_SIZE, FLASH_SIZE

SMOKE = []


def scenario(fn):
    SMOKE.append(fn)
    return fn


class Lab:
    def __init__(self, folder, binary):
        self.root, self.binary = Path(folder), binary
        self.flash = self.root / "device.flash"
        self.images = {}
        for version in range(1, 5):
            self.images[version] = self.root / f"v{version}.img"
            self.images[version].write_bytes(package(bytes((version + i * 17) & 255 for i in range(67)), version))
        self.call("init", self.flash)

    def call(self, *args, expected=0):
        p = subprocess.run([self.binary, *map(str, args)], capture_output=True, text=True, timeout=10)
        if p.returncode != expected:
            raise AssertionError(f"{args}: expected exit {expected}, got {p.returncode}: {p.stdout} {p.stderr}")
        if expected == 86:
            if p.stdout:
                raise AssertionError("cut returned a completed command response")
            return None
        if p.stderr:
            raise AssertionError(f"unexpected diagnostic: {p.stderr}")
        return json.loads(p.stdout)

    def provision(self):
        return self.call("provision", self.flash, self.images[1])

    def stage(self, version=2):
        return self.call("stage", self.flash, self.images[version])

    def boot(self):
        return self.call("boot", self.flash)

    def confirm(self, slot=1, version=2):
        return self.call("confirm", self.flash, slot, version)

    def check_selection(self, choice, versions, recovery=False):
        if choice["decision"] == "recovery":
            assert recovery, choice
            assert "slot" not in choice
            return
        assert choice["decision"] in ("confirmed", "trial"), choice
        assert choice["version"] in versions, choice
        raw = self.flash.read_bytes()
        offset = SLOT_OFFSETS[choice["slot"]]
        info, _ = parse(raw[offset:offset + SLOT_SIZE])
        for field in ("version", "length", "payload_crc"):
            assert info[field] == choice[field], (field, info, choice)

    def flip(self, offset):
        data = bytearray(self.flash.read_bytes())
        data[offset] ^= 1
        self.flash.write_bytes(data)


@scenario
def invalid_fault_options_do_not_mutate(lab):
    before = lab.flash.read_bytes()
    for options in (("--cut-at", "0"), ("--fail-at", "0"), ("--tear", "1"),
                    ("--fail-at", "1", "--cut-at", "1"), ("--fail-at", " 1"),
                    ("--fail-at", "-1"), ("--fail-at", "4294967296")):
        result = subprocess.run([lab.binary, "boot", str(lab.flash), *options],
                                text=True, capture_output=True, timeout=3)
        assert result.returncode == 2
        assert lab.flash.read_bytes() == before


@scenario
def blank_recovery_and_exclusive_create(lab):
    before = lab.flash.read_bytes()
    lab.call("init", lab.flash, expected=2)
    assert lab.flash.read_bytes() == before
    lab.check_selection(lab.boot(), (), recovery=True)


@scenario
def confirmed_update(lab):
    lab.provision()
    lab.check_selection(lab.boot(), {1})
    lab.stage()
    trial = lab.boot()
    assert trial["decision"] == "trial" and trial["slot"] == 1
    lab.check_selection(trial, {2})
    lab.confirm()
    stable = lab.boot()
    assert stable["decision"] == "confirmed"
    lab.check_selection(stable, {2})


@scenario
def unconfirmed_trial_rolls_back(lab):
    lab.provision(); lab.stage()
    lab.check_selection(lab.boot(), {2})
    back = lab.boot()
    assert back["reason"] == "unconfirmed_trial"
    lab.check_selection(back, {1})
    lab.check_selection(lab.boot(), {1})


@scenario
def confirmation_requires_matching_trial(lab):
    lab.provision(); lab.stage()
    before = lab.flash.read_bytes()
    lab.call("confirm", lab.flash, 1, 2, expected=2)
    assert lab.flash.read_bytes() == before
    lab.boot()
    before = lab.flash.read_bytes()
    lab.call("confirm", lab.flash, 0, 2, expected=2)
    lab.call("confirm", lab.flash, 1, 3, expected=2)
    assert lab.flash.read_bytes() == before
    lab.confirm(); lab.check_selection(lab.boot(), {2})


@scenario
def reject_packages_before_mutation(lab):
    lab.provision()
    good = lab.images[2].read_bytes()
    bad_header = bytearray(good); bad_header[12] ^= 1
    bad_payload = bytearray(good); bad_payload[-1] ^= 1
    bad_reserved = bytearray(good); bad_reserved[28] = 1
    bad_length = bytearray(good); struct.pack_into("<I", bad_length, 16, 4065)
    struct.pack_into("<I", bad_length, 24, zlib.crc32(bad_length[:24]))
    invalid = [b"short", good[:-1], good + b"extra", bad_header, bad_payload,
               bad_reserved, bad_length, package(b"wrong", 2, target=123),
               lab.images[1].read_bytes(), b"X" * 4097]
    before = lab.flash.read_bytes()
    for data in invalid:
        path = lab.root / "invalid.img"; path.write_bytes(data)
        result = lab.call("stage", lab.flash, path, expected=2)
        assert result["operations"] == 0 and lab.flash.read_bytes() == before


@scenario
def pending_update_is_not_overwritten(lab):
    lab.provision(); lab.stage()
    before = lab.flash.read_bytes()
    lab.call("stage", lab.flash, lab.images[3], expected=2)
    assert lab.flash.read_bytes() == before


@scenario
def repeated_updates_reuse_both_slots(lab):
    lab.provision()
    for version in (2, 3, 4):
        slot = (version - 1) % 2
        lab.stage(version)
        lab.check_selection(lab.boot(), {version})
        lab.confirm(slot, version)
        lab.check_selection(lab.boot(), {version})


@scenario
def candidate_corruption_falls_back(lab):
    lab.provision(); lab.stage()
    lab.flip(SLOT_OFFSETS[1] + 32)
    selected = lab.boot()
    assert selected["reason"] == "candidate_invalid"
    lab.check_selection(selected, {1})


@scenario
def confirmed_corruption_enters_recovery(lab):
    lab.provision(); lab.stage()
    lab.flip(SLOT_OFFSETS[0] + 32)
    selected = lab.boot()
    assert selected["decision"] == "recovery"


@scenario
def metadata_corruption_fallback_and_recovery(lab):
    lab.provision(); lab.stage()
    state = lab.call("status", lab.flash)
    latest = state["metadata_page"]
    lab.flip(latest * 256 + 52)
    lab.check_selection(lab.boot(), {1})
    lab.flip((1 - latest) * 256 + 52)
    assert lab.boot()["decision"] == "recovery"


@scenario
def image_identity_must_match_metadata(lab):
    lab.provision(); lab.stage()
    blob = bytearray(lab.flash.read_bytes())
    other = lab.images[3].read_bytes()
    offset = SLOT_OFFSETS[1]
    blob[offset:offset + SLOT_SIZE] = b"\xff" * SLOT_SIZE
    blob[offset:offset + len(other)] = other
    lab.flash.write_bytes(blob)
    selected = lab.boot()
    assert selected["reason"] == "candidate_invalid"
    lab.check_selection(selected, {1})


@scenario
def file_size_and_process_lock(lab):
    bad = lab.root / "bad.flash"; bad.write_bytes(b"\xff" * (FLASH_SIZE - 1))
    lab.call("status", bad, expected=2)
    with lab.flash.open("r+b") as f:
        fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        lab.call("status", lab.flash, expected=2)
    lab.call("status", lab.flash)


@scenario
def image_capacity_and_padding(lab):
    lab.provision()
    for version, size in ((2, 4064), (3, 1), (4, 3)):
        path = lab.root / f"boundary-{version}.img"
        path.write_bytes(package(bytes([version]) * size, version))
        result = lab.call("stage", lab.flash, path)
        slot = result["slot"]
        lab.check_selection(lab.boot(), {version})
        lab.confirm(slot, version)
        raw = lab.flash.read_bytes()
        end = SLOT_OFFSETS[slot] + IMAGE_HEADER + size
        if size % 4:
            assert raw[end:end + 4 - size % 4] == b"\xff" * (4 - size % 4)


@scenario
def metadata_generation_cannot_wrap(lab):
    lab.provision()
    raw = bytearray(lab.flash.read_bytes())
    struct.pack_into("<Q", raw, 8, 2**64 - 1)
    struct.pack_into("<I", raw, 52, zlib.crc32(raw[:52]))
    lab.flash.write_bytes(raw)
    before = lab.flash.read_bytes()
    r = lab.call("stage", lab.flash, lab.images[2], expected=2)
    assert r["operations"] == 0 and lab.flash.read_bytes() == before
    lab.check_selection(lab.boot(), {1})


@scenario
def ambiguous_metadata_is_rejected(lab):
    lab.provision()
    raw = bytearray(lab.flash.read_bytes()); raw[256:320] = raw[:64]
    lab.flash.write_bytes(raw)
    assert lab.boot()["decision"] == "recovery"


@scenario
def reprovision_is_not_an_erase_command(lab):
    lab.provision()
    before = lab.flash.read_bytes()
    lab.call("provision", lab.flash, lab.images[2], expected=2)
    assert lab.flash.read_bytes() == before


IMAGE_HEADER = 32


def campaign(lab, name, fault="cut"):
    if name == "provision":
        command = ("provision", lab.flash, lab.images[1])
        expected, allow_recovery, active_offset = {1}, True, None
        metadata_erase = 16 + (len(lab.images[1].read_bytes()) - IMAGE_HEADER + 3) // 4 + 8 + 1
        erase_indices = set(range(1, 17)) | {metadata_erase}
    else:
        lab.provision()
        if name == "stage":
            lab.stage(); lab.boot(); lab.confirm()
            command = ("stage", lab.flash, lab.images[3])
            expected, allow_recovery, active_offset = {2, 3}, False, SLOT_OFFSETS[1]
            metadata_erase = 16 + (len(lab.images[3].read_bytes()) - IMAGE_HEADER + 3) // 4 + 8 + 1
            erase_indices = set(range(1, 17)) | {metadata_erase}
        else:
            lab.stage()
            if name in ("confirm", "rollback"):
                lab.boot()
            command = ("confirm", lab.flash, 1, 2) if name == "confirm" else ("boot", lab.flash)
            expected = {1} if name == "rollback" else {1, 2}
            allow_recovery, active_offset = False, SLOT_OFFSETS[0]
            erase_indices = {1}
    baseline = lab.flash.read_bytes()
    complete = lab.call(*command)
    operations = complete["operations"]
    expected_operations = max(erase_indices) + 15 if name in ("provision", "stage") else 16
    assert operations == expected_operations
    trials = []
    for index in range(1, operations + 1):
        prefixes = (0, 1, 127, 255, 256) if index in erase_indices else (0, 1, 2, 3, 4)
        for prefix in prefixes:
            lab.flash.write_bytes(baseline)
            response = lab.call(*command, "--cut-at" if fault == "cut" else "--fail-at", index,
                                "--tear", prefix, expected=86 if fault == "cut" else 2)
            if fault == "io":
                assert response["result"] == "error" and response["operations"] == index
            raw = lab.flash.read_bytes()
            if active_offset is not None:
                assert raw[active_offset:active_offset + SLOT_SIZE] == baseline[active_offset:active_offset + SLOT_SIZE]
            choice = lab.boot()
            lab.check_selection(choice, expected, recovery=allow_recovery)
            if choice["decision"] == "trial":
                lab.check_selection(lab.boot(), {1} if name != "stage" else {2})
            trials.append({"operation": index, "prefix_bytes": prefix, "decision": choice["decision"],
                           "version": choice.get("version")})
    return {"name": name, "fault": fault, "operations": operations, "cuts_tested": len(trials), "cases": trials}


def main():
    p = argparse.ArgumentParser(description="Run state-policy scenarios and interrupted-Flash campaigns.")
    p.add_argument("--binary", default="build/fwctl")
    p.add_argument("--report", default="build/integration-results.json")
    p.add_argument("--smoke-only", action="store_true")
    a = p.parse_args()
    binary = str(Path(a.binary).resolve())
    results, campaigns = [], []
    Path("build").mkdir(exist_ok=True)
    for test in SMOKE:
        begin = time.monotonic()
        try:
            with tempfile.TemporaryDirectory(prefix="update-test-", dir="build") as folder:
                test(Lab(folder, binary))
            result = {"name": test.__name__, "passed": True}
        except Exception as error:
            traceback.print_exc()
            result = {"name": test.__name__, "passed": False, "error": str(error)}
        result["wall_seconds"] = round(time.monotonic() - begin, 3)
        results.append(result)
        print("PASS" if result["passed"] else "FAIL", test.__name__, flush=True)
    if not a.smoke_only:
        for fault, name in ((fault, name) for fault in ("cut", "io")
                            for name in ("provision", "stage", "trial", "confirm", "rollback")):
            try:
                with tempfile.TemporaryDirectory(prefix="update-cut-", dir="build") as folder:
                    result = campaign(Lab(folder, binary), name, fault)
                result["passed"] = True
            except Exception as error:
                traceback.print_exc()
                result = {"name": name, "fault": fault, "passed": False, "error": str(error)}
            campaigns.append(result)
            print("PASS" if result["passed"] else "FAIL", fault, name, "campaign", result.get("cuts_tested", 0), flush=True)
    passed = all(r["passed"] for r in results + campaigns)
    report = {"passed": passed, "scenarios": results, "campaigns": campaigns,
              "power_cuts_tested": sum(r.get("cuts_tested", 0) for r in campaigns if r.get("fault") == "cut"),
              "io_failures_tested": sum(r.get("cuts_tested", 0) for r in campaigns if r.get("fault") == "io"),
              "binary_sha256": hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
              "scope": "File-backed prefix-tear Flash model; not actual MCU power-loss or security validation"}
    target = Path(a.report); target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Scenarios: {sum(r['passed'] for r in results)}/{len(results)}; "
          f"campaigns: {sum(r['passed'] for r in campaigns)}/{len(campaigns)}; "
          f"power cuts: {report['power_cuts_tested']}; returned errors: {report['io_failures_tested']}")
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
