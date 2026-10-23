# Firmware Update and Recovery Engine

A C implementation of A/B image staging, trial boot selection, application
confirmation, and rollback over a persistent Flash model. Python tests interrupt
storage operations and independently validate the image selected after restart.

The engine keeps a confirmed image while an update is written to the other slot.
Two metadata pages use generation numbers, CRCs, and a final commit word. A partial
new record is rejected without replacing the previous committed record.

This is a host-executed update/boot-policy model. `boot` returns an image selection;
it does not execute payload bytes or jump to a microcontroller reset vector.

## Build

Requirements: a C11 compiler, Make, and Python 3 on macOS or Linux. The tests use
POSIX file locks. There are no third-party libraries or Python packages.

```sh
make
make demo
make test
```

Clang is the default compiler. `make CC=gcc` selects another compatible compiler.
AddressSanitizer and UndefinedBehaviorSanitizer are required for the sanitizer
targets. GitHub Actions runs the same suite on Ubuntu.

## Behavior

```text
Validated package
       |
Erase and write inactive image slot
       |
Verify stored header and payload
       |
Commit pending-image metadata
       |
boot: persist trial-attempt state before selection
       |
       +-- matching confirmation --> new confirmed image
       |
       +-- next boot without confirmation --> old confirmed image
```

No metadata commit means no automatic activation, even if a slot happens to
contain a valid image. Missing/unusable metadata or an invalid confirmed image
produces an explicit recovery decision.

## CLI

```text
fwctl init FLASH
fwctl provision FLASH IMAGE
fwctl stage FLASH IMAGE
fwctl boot FLASH
fwctl confirm FLASH SLOT VERSION
fwctl status FLASH
```

Commands return one JSON object. Success exits 0; rejected input or an I/O error
exits 2. A recovery decision is a successful boot-policy result, not an error.
Simulated power interruption exits 86 without a completed command response.

Create packages and run a confirmed update from a fresh build directory:

```sh
python3 tools/make_image.py --version 1 --size 67 --output build/v1.img
python3 tools/make_image.py --version 2 --size 67 --output build/v2.img
build/fwctl init build/device.flash
build/fwctl provision build/device.flash build/v1.img
build/fwctl stage build/device.flash build/v2.img
build/fwctl boot build/device.flash
build/fwctl confirm build/device.flash 1 2
build/fwctl boot build/device.flash
```

`init` and the package generator do not overwrite existing output files.
`provision` requires completely erased Flash. `stage` requires a confirmed valid
image, no pending update, and a strictly newer version. A pending image cannot be
confirmed before it has been selected for trial. Confirmation must match its
slot and version.

To demonstrate rollback, omit confirmation and call `boot` again. `make demo`
performs both flows in a temporary directory and also interrupts a later update.

## Fault injection

Mutation commands accept `--cut-at N` and optional `--tear BYTES`. Operations are
numbered from one within each command. At operation N, only the specified prefix
is persisted and synchronized, then the process exits immediately. With no tear
argument, the complete operation is persisted before exit.

```sh
python3 tools/make_image.py --version 3 --output build/v3.img
build/fwctl stage build/device.flash build/v3.img --cut-at 18 --tear 2
build/fwctl boot build/device.flash
```

The last command still selects confirmed version 2 for this interrupted transfer.
The file emulates 256-byte erase pages, aligned four-byte programming, and
programming transitions from 1 to 0 only. It is not a model of arbitrary physical
bit damage or an actual operating-system power failure.

`--fail-at N [--tear BYTES]` persists the selected prefix and returns an I/O error
instead of terminating. The CLI reports exit code 2 and an error result. It cannot
be combined with `--cut-at`. A complete commit may reach storage before the error
is reported, so callers must inspect durable state before retrying.

## Verification

```sh
make test-unit
make test-integration
make test-sanitized
```

The suite includes nine sanitized C test groups, seventeen CLI policy scenarios,
and ten fault campaigns. The power-interruption campaigns cover **810 cut cases**,
repeated as **810 returned-error cases**:
every mutation position, every four-byte programming prefix length from zero to
four, and selected erase prefixes. Sanitized CLI scenarios run separately.

An independent Python parser checks selected image headers and payload CRCs.
Tests also verify that the confirmed slot is not modified during staging and that
invalid packages cause no storage mutations. Machine-readable results are written
under `build/`; captured results are retained in `evidence/`.

## Repository

| Path | Responsibility |
|---|---|
| `src/flash.c` | Persistent I/O, geometry, process locking, fault injection |
| `src/format.c` | CRC, image verification, metadata encoding and journal commit |
| `src/main.c` | CLI, update policy, boot selection, confirmation |
| `include/fw.h` | Shared types, layout constants and API |
| `tools/make_image.py` | Package generator, including external payload support |
| `tools/image_format.py` | Independent Python package format implementation |
| `tools/demo.py` | Reproducible update/rollback/interruption demonstration |
| `tests/` | C tests, CLI scenarios, restart oracle and cut campaigns |
| `docs/` | Storage format, policy, assumptions and verification matrix |
| `evidence/` | Recorded results and build environment |

## Scope

CRC provides accidental-corruption detection, not authentication. Version checks
are an update policy, not tamper-resistant anti-rollback. Payloads are data; vector
validation, MCU handoff, watchdog integration, cryptographic verification and
hardware Flash drivers are outside this implementation.

Recovery depends on the stated prefix-tear model and the continued validity of
the confirmed slot and one committed metadata record. Initial provisioning can
end in recovery if interrupted; automatic reprovisioning is not implemented.
The engine stops rather than guessing when metadata is absent or ambiguous.

See [architecture](docs/architecture.md), [storage format](docs/storage-format.md),
and [verification](docs/verification.md). Application source is MIT licensed.
