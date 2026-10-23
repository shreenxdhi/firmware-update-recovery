# Recorded validation

The final C engine passed a clean local `make test` on the compiler and host recorded in `environment.json`.

| Layer | Result |
| --- | --- |
| C core under ASan/UBSan | 9/9 test groups |
| Optimized CLI scenarios | 17/17 |
| Power-cut campaigns | 5/5, 810 injected cuts |
| Returned mutation-error campaigns | 5/5, 810 injected errors |
| Sanitized CLI scenarios | 17/17 |
| Confirmation, rollback and interrupted-update demonstration | Completed |

Each fault mode covers provisioning (285 cases), staging into a reused inactive slot (285), trial recording (80), confirmation (80) and rollback (80). Every selected mutation position is tested. Program prefixes are 0, 1, 2, 3 and 4 bytes; erase prefixes are 0, 1, 127, 255 and 256 bytes. The independent Python struct/zlib oracle checks selected image identity and confirmed-slot preservation. Trials are checked for rollback on a subsequent unconfirmed boot. Returned errors require an error response rather than a successful decision, even when a full commit has reached durable storage.

Additional checks cover all 512 single-bit corruptions of a metadata record, package/header bounds, generation exhaustion, process locks, version policy, repeated slot reuse, invalid fault options and rejection before mutation. No sanitizer finding occurred in exercised paths. Host C static analysis completed without diagnostics on format.c, flash.c and main.c.

`test-results.log` contains the clean-build output. `integration-results.json` records all 1,620 injected-fault outcomes and the binary hash; `sanitizer-results.json` records the sanitized CLI scenarios. `demo-transcript.txt` records the demonstration. `environment.json` records tool versions and the tested binary hash.

This is a file-backed prefix-persistence model. It does not prove physical Flash behavior, read-error handling, kernel short-write behavior, arbitrary bit-damage resilience, cryptographic authenticity or actual MCU boot/application execution. Hosted CI is configured but has not been run here.
