# Architecture

## Storage and policy layers

The storage layer exposes bounds-checked reads, aligned programming, and page
erase. Successful mutations synchronize the backing file before returning. The
CLI takes a nonblocking POSIX write lock for the lifetime of its file access so
separate engine processes cannot operate concurrently on the same file.

The format layer serializes integers explicitly, validates CRCs and semantics,
and loads the newest valid metadata generation. The policy layer decides which
slot may be written and selected. Package validation completes in RAM before
any erase/program operation begins.

The Flash creation command uses exclusive creation. Existing files are never
implicitly truncated. Open requires a regular file of exactly 8,704 bytes. The
package reader bounds its input to one slot and rejects extra trailing bytes.

## Staging order

1. Validate the complete package and existing confirmed image.
2. Reject pending updates, non-newer versions, and exhausted generation numbers.
3. Erase all pages of the inactive slot.
4. Program the payload, padding the final program word with erased bytes.
5. Program the image header.
6. Read back and validate the complete image and expected identity.
7. Write a pending-image metadata snapshot to the alternate metadata page.

The active image slot is not written by this flow. An interrupted inactive image
is ignored unless a complete committed record references its validated identity.
The candidate may replace an older retired image, which is exercised by the
staging campaign rather than testing only an initially empty inactive slot.

## Metadata commit

The engine reads both metadata pages. Records need valid magic/format, semantic
fields, CRC, exact commit word, and reserved fields. If both are valid, the greater
generation wins. Equal generations are treated as ambiguous and cause recovery.
Generation increment is checked against UINT64_MAX instead of wrapping.

A transition erases the page that does not contain the selected record. It then
programs fourteen four-byte words covering the record body and CRC, followed by
the commit word. The previous record remains untouched. The saved record is
reloaded to verify its generation and page before returning success.

A partial commit is valid only if its stored bytes exactly match the commit word
and the record passes all other checks. All four commit bytes differ from 0xFF,
so a strict prefix tear of that word cannot accidentally look complete.

## Boot state machine

| Current state | Additional condition | Persistent transition | Decision |
|---|---|---|---|
| No usable metadata | Any slot contents | None | Recovery |
| Confirmed image invalid | Even with a valid candidate | None | Recovery |
| No pending image | Confirmed image valid | None | Confirmed |
| Pending, not attempted | Candidate valid and identity matches | Mark attempted | Trial candidate |
| Pending, not attempted | Candidate invalid or identity differs | Clear pending | Confirmed fallback |
| Pending, attempted | No accepted confirmation | Clear pending | Confirmed rollback |
| Pending, attempted | Explicit matching confirmation | Promote candidate, clear pending | New confirmed |

The attempt record must commit before a trial decision is returned. Interruption
before that commit leaves a not-yet-attempted candidate. Interruption after commit
but before the response is observed causes a conservative rollback next time.
Exactly-once delivery of a boot decision is not promised across process failure.

Confirmation is a separate application-facing operation. The model validates the
pending slot/version/CRC/length identity and trial state. It does not execute or
assess application health; the caller is responsible for invoking confirmation
after its intended checks. This is a policy interface, not a security boundary.

## Failure model

Each modeled erase or program operation can finish fully or persist a byte prefix
and terminate the process. All earlier successful operations remain durable.
Bytes after the prefix retain their prior state. Program changes only 1 to 0;
erase changes the modeled prefix to 0xFF. The test process restarts a new engine
against the resulting file.

This is not a complete physical Flash fault model. Real hardware can have
device-specific write units, ECC, disturbed bits, supply constraints and partial
erase behavior unlike a prefix. The host's fsync behavior is assumed; the harness
does not cut power to the host or test controller/disk caches and filesystem
recovery. File/directory creation durability is not part of the campaign.

The guarantee under this model is bounded: during a normal update transition,
the unchanged confirmed image and a usable committed record remain available.
Selection returns a verified permitted image or explicit recovery. Simultaneous
corruption of the confirmed image and records is not covered by an availability
claim. Corruption tests exercise fail-closed decisions and selected-record fallback.

## Operational limits

- One pending update at a time; no transport, compression or delta patches.
- No garbage-collected update log; two pages trade simple recovery for erase wear.
- No protected monotonic counter or signed image trust chain.
- No automatic scan-and-activate fallback when metadata is lost.
- Initial provisioning requires entirely blank storage. Interrupted provisioning
  may require a fresh Flash file in this host model.
- Process locks coordinate compliant engine processes, not arbitrary external
  writers or threads using this API concurrently.
- JSON stdout is a command result, not an atomic transaction receipt; lost output
  after a committed transition is resolved by querying state.
