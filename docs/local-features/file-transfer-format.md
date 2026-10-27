# Encrypted local-data file transfer, version 1

This is the Desktop file-transfer contract. It does not implement QR pairing,
network transport, interrupted-transfer resumption, or an Android importer.
Those remain separate LF-01 deliverables. A transfer contains the local module
registry and its resources, including unsupported modules; it is not a Telegram
session backup or an export of history that the client never downloaded.

The file extension is `.tgv`. The export password is independent of Telegram
credentials. Passwords are encoded as UTF-8 exactly as entered, without Unicode
normalization. Desktop requires at least 12 characters when creating a file and
accepts at most 1024 UTF-8 bytes. A receiving client must apply the same UTF-8
encoding. Telegram authentication keys, session files, system-keystore secrets,
and device executor activation are not inputs to this archive.

## Encryption envelope

All integer fields in the binary envelope are unsigned, big-endian integers.

| Offset | Bytes | Value |
| --- | ---: | --- |
| 0 | 8 | ASCII `TGVXFR01` |
| 8 | 4 | PBKDF2 iteration count, exactly `600000` in version 1 |
| 12 | 16 | Cryptographically random salt |
| 28 | 12 | Cryptographically random AES-GCM nonce |
| 40 | variable | Encrypted package stream |
| end minus 16 | 16 | AES-GCM authentication tag |

Derive a 32-byte key with PBKDF2-HMAC-SHA256 using the UTF-8 password, salt and
iteration count above. Encrypt using AES-256-GCM. The complete 40-byte header is
authenticated additional data. The nonce is used once with that derived key.
There is one GCM operation for the package, without compression. A reader rejects
unknown magic or KDF parameters rather than guessing a fallback cipher. Salt,
nonce and keys come from OpenSSL's cryptographic random generator. Keys and
temporary password byte buffers are cleared when their operation completes.

## Authenticated package stream

The decrypted stream is:

1. Eight ASCII bytes `TGVPKG01`.
2. A four-byte big-endian manifest length.
3. That many bytes of UTF-8 JSON containing `manifest.json`.
4. The exact bytes of each entry in the manifest's `files` array, in array order.

There are no tar headers, zip entries, compression, symlinks, or implicit filesystem
extraction. Entries have this logical layout:

```text
manifest.json
profiles/<profile-uuid>/account.json
profiles/<profile-uuid>/modules/<module_id>.json
assets/<sha256>
```

The manifest has these known fields:

| Field | Meaning |
| --- | --- |
| `format` | Literal `telegravim-transfer` |
| `version` | Integer `1` |
| `package_id` | Fresh lowercase, hyphenated UUID for this export |
| `created_at` | Export time in UTC ISO 8601; not a conflict-ordering clock |
| `device_id` | Source local-data device UUID |
| `generation` | Source consistent-snapshot generation UUID |
| `extra` | Preserved store metadata |
| `retained_transfer` | Opaque siblings of the snapshot's structural transfer metadata |
| `profiles` | Profile registry described below |
| `assets` | Resource registry described below |
| `files` | Ordered list of `{path, size, sha256}` |

`size` is a canonical nonnegative decimal string. `sha256` is 64 lowercase
hexadecimal characters. File SHA-256 covers plaintext entry bytes; GCM covers the
entire stream and header. The final byte of the final entry must be the final
byte of the plaintext stream.

Each profile registry object contains `profile_id`, the exact `account` entry
path, and a `modules` array of `{module_id, path}`. Account JSON contains
`profile_id`, `environment`, `user_id`, an `extra` object, and a
`retained_transfer` object. Account matching
uses environment and stable decimal Telegram user ID. It never maps accounts by
their tab position, display name, directory, or profile UUID alone.

Each asset registry object contains `sha256`, decimal-string `size`, `extra`,
and `retained_transfer`.
The corresponding entry must be exactly `assets/<sha256>` and must match the
registered size and digest. Module entry bytes contain the module's own
`module_id`, `schema_version`, records and unknown fields. The module ID must
match the registry. Supported-module validation and version-vector merging are
provided by the common local-data store.

UUIDs are lowercase and include hyphens. Module IDs match
`[a-z0-9_]{1,96}`. Every entry path must exactly equal a path constructed
from its validated registry identities. Duplicate paths, duplicate registry
identities, unregistered files, missing files and extra stream bytes are rejected.
Absolute paths and traversal components cannot enter filesystem operations.

## Unknown metadata

Unknown fields are retained at the manifest, account, profile-registry,
module-registry, asset-registry and file-entry levels. The store retains these in
its separate opaque `transfer` metadata, apart from feature `extra` data:

- Snapshot: `manifest`.
- Profile: `account`, `profile`, `modules[module_id]`,
  `files.account` and `files.modules[module_id]`.
- Asset: `registry` and `file`.

Other siblings in each `transfer` object, including saved metadata conflict
variants, are carried in a reserved `retained_transfer` object: in the manifest
for a snapshot, in account JSON for a profile, and in the asset registry for an
asset. This field may be absent when reading an earlier package, which means
there are no retained siblings. A present field must be an object and must not
contain the structural keys for its scope (`manifest` for a snapshot;
`account`, `profile`, `modules`, or `files` for a profile; `registry` or `file`
for an asset). Import restores these siblings beside the structural metadata.
It excludes the reserved wire field from unknown-field capture, so repeated
export/import cycles do not wrap or duplicate retained metadata. Retained
metadata is subject to the same JSON size and nesting limits as its container.

File metadata is attached to the logical object rather than an old profile path,
so matching an imported account to an existing profile can retain that metadata.
Export restores unknown fields at their original levels and then assigns every
known field from validated state. Opaque fields cannot override canonical
identities, sizes, digests or file paths. Unknown module JSON is retained without
being executed. Import does not enable an automation executor.

## Bounds and application

The current implementation limits a package stream to 33 GiB, registered assets
to 32 GiB in total and individually, each JSON entry to 16 MiB, aggregate JSON
entries to 256 MiB, the manifest to 32 MiB, and the entry count to 116512. JSON nesting
is limited to 32 levels. Limits are checked before buffers or asset writes grow
past them; a limit failure is not a partial successful transfer. Large-resource
I/O uses chunks of at most 256 KiB. The whole archive is never loaded into memory.

Export reads one immutable store snapshot, verifies each resource's declared
size and digest as it streams, and writes through `QSaveFile`. Cancellation or
an error leaves the existing destination untouched. Completion atomically
replaces the destination only after the tag has been written successfully.

Import uses two passes over the same open input file:

1. Authenticate the entire GCM stream while discarding plaintext. Also compute
   the ciphertext-and-header SHA-256.
2. Independently authenticate again while parsing bounded JSON in memory and
   streaming resources into an encrypted quarantine. Require the same encrypted
   digest and file size before presenting an import preview.

Quarantine resources use separate AES-256-GCM streams with fresh random nonces
and an ephemeral random key held only in memory. Their plaintext SHA-256 is AAD.
Only ciphertext and authentication tags reach temporary files. Quarantine
readers decrypt directly into the store's pending encrypted transaction and
verify both the tag and content digest. Temporary files are removed when their
reader ownership ends; a crashed process can leave only encrypted files whose
ephemeral key was not persisted.

The preview shows package size, resources, account mapping, module identities,
unsupported modules, and concurrent record versions. Account data for identities
not signed in on this device is retained without logging into those accounts.
Conflicts default to retaining the current version and archiving the alternate,
including when the current record is deleted. Per-conflict choices can instead
retain only the current or incoming version. Saved conflict versions are
discoverable from the transfer screen, with paginated inspection and an explicit
restore action. Restoring an alternate preserves the previously current version
and uses the store generation check and version-vector update. The common store applies the reviewed plan with
a generation check and one atomic commit. Stale previews require a new inspection.
Cancellation, authentication failure, a changed source, failed asset reads, or a
failed atomic commit must not publish a partially imported generation.

Build success, a valid encrypted file and a green code review are not proof of an
Android round trip. Cross-client compatibility and actual QR transfer require
the later Android implementation and direct verification.
