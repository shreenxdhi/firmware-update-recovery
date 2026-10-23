# Verification

## Acceptance properties

| ID | Property | Tests |
|---|---|---|
| U01 | Existing files are not truncated by init | Exclusive-create scenario |
| U02 | Image/header bounds and CRCs are checked before mutation | Invalid-package scenario; C header tests |
| U03 | Flash enforces aligned programming, bounds and 1-to-0 transitions | Sanitized C Flash tests |
| U04 | Staging does not modify the confirmed slot | Every stage cut case compares its exact bytes |
| U05 | An image is selected only with valid committed metadata and matching identity | Python selection oracle; identity/corruption tests |
| U06 | Trial attempt is persisted before selection | Trial-write campaign |
| U07 | An unconfirmed trial rolls back | Policy scenario and rollback campaign |
| U08 | Confirmation requires a matching attempted candidate | Confirmation guards and interruption campaign |
| U09 | Previous metadata survives an incomplete replacement | Metadata tests and every transition campaign |
| U10 | Lost/ambiguous metadata results in recovery | Corruption and equal-generation scenarios |
| U11 | Image slots can be reused safely | Repeated updates and stage campaign with occupied inactive slot |
| U12 | Generation cannot wrap and version downgrade is rejected | Generation/version scenarios |
| U13 | Concurrent engine processes cannot mutate one open Flash file | Separate-process POSIX locking scenario |

## Test layers

The C unit binary exercises CRC reference vectors, serialization, program/erase
rules, metadata validation and fallback, header limits, and generation exhaustion
under AddressSanitizer and UndefinedBehaviorSanitizer. It rejects every single-bit
corruption of a sample 64-byte metadata record, including the commit and reserved
fields. These tests do not imply detection of every possible multi-bit pattern.

Sixteen black-box scenarios invoke the actual C CLI. The Python package encoder
and selection oracle use struct and zlib rather than the C format routines.
Invalid-package tests compare the complete file before/after rejection and require
zero modeled mutations. A sanitizer-built CLI runs all sixteen scenarios again.

## Power-interruption campaigns

Each case restores the exact baseline file, invokes one command with a cut
location, waits for exit 86, then starts a separate process for boot selection.
The selected slot is parsed independently and its CRC/length/version compared
with the reported selection. Any invalid/unexpected selection fails the test.

| Campaign | Baseline | Command interrupted | Cases |
|---|---|---|---|
| Provision | Blank Flash | Initial image and confirmed record | 285 |
| Stage | Confirmed v2; retired v1 in inactive slot | Replace inactive image with v3 | 285 |
| Trial | Confirmed v1 plus staged v2 | Mark trial attempted | 80 |
| Confirm | v2 trial already selected | Promote v2 | 80 |
| Rollback | v2 trial not confirmed | Clear pending reference | 80 |

Total: 810 cuts. Every mutation position is tested. All five prefix lengths for a
four-byte programming operation are covered. Erase prefixes are sampled at 0, 1,
127, 255 and 256 bytes; intermediate erase lengths and arbitrary bit patterns are
not exhaustive. The harness checks operation counts against the expected plan so
layout/write-order changes cannot silently use stale campaign geometry.

Interrupted provision is permitted to return recovery because no confirmed image
existed beforehand. Stage/trial/confirmation/rollback tests require a valid permitted
selection. If the restart selects a trial, another boot without confirmation must
return the prior confirmed version. Confirmed slot bytes must remain unchanged
through the injected transition.

## Evidence and limitations

`make test` returns nonzero if an assertion, sanitizer check, policy scenario, or
campaign fails. JSON results include per-case decisions and the executable hash.
Wall durations measure host test execution, not hardware Flash timing. Recorded
results in evidence describe the captured build; new runs write under build.

The model does not execute firmware payloads, simulate an MCU reset vector,
physically interrupt a power rail, authenticate packages, or establish compliance
with a safety/security standard. Arbitrary storage damage, read failures, kernel short-write injection,
device-specific ECC, endurance and interrupted initial file creation
are outside the cut campaigns. Hosted CI configuration is separate from evidence
that a remote CI run has completed.


## Returned mutation failures

`--fail-at N [--tear BYTES]` injects a returned I/O error at the Nth program/erase mutation. It persists the requested prefix using the same Flash model, synchronizes it, and returns failure instead of terminating the process. The command returns JSON error status and exit code 2. `--cut-at` and `--fail-at` are mutually exclusive. A failed command can leave a fully persisted commit: callers must inspect durable state rather than assume an error means nothing changed.

The full regression now repeats all five power-cut campaigns as returned-error campaigns, covering 810 power cuts and 810 returned mutation failures. It verifies confirmed-slot preservation, independently validates selected image identity, and checks rollback after an unconfirmed trial. This injection covers mutation return handling with prefix persistence; it does not emulate failed reads, kernel short writes, asynchronous storage faults, real flash wear or adversarial image authenticity.
