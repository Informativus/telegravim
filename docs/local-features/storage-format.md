# Local data format, version 1

This document describes the LF-01a implementation contract. It does not claim
user acceptance, successful runtime validation, QR transfer, or Android support.
The complete feature requirements remain in [the specification](specification.md).

## Portable data

[The JSON Schema](local-data.schema.json) describes a module at its root.
`#/$defs/manifest` and `#/$defs/accountFile` describe the transfer manifest and
account files. All envelopes permit unknown properties. JSON is UTF-8, without
a byte-order mark. Equality after transfer means equal JSON values and links;
whitespace and object property ordering are not significant.

The identity of an account is `(environment, user_id)`, with environment
`production` or `test` and a positive decimal user ID string. A profile UUID is
a container identity, not an alternative account identity. Import maps equal
account identities together; an unrelated profile UUID collision gets a fresh
UUID. A message reference includes its account through its enclosing profile,
peer type, decimal peer ID, and decimal message ID. Desktop pointers, account
slots, packed peer identifiers, and filesystem locations are not portable IDs.

Every registered module declares an ID, current schema version, semantic
validator, and resource-reference enumerator. The first registered module is
`saved_tags`, current version 2 with explicit version 1 compatibility. It contains `records`, an array of objects with UUID
`id`, `version_vector`, and Boolean `deleted`. Its active record kinds are:

- `tag`: `title`, original `emoji`, `position`, and optional `native_reaction`.
- `binding`: `tag_id`, `message`, optional saved `source_text`/`source_hash`,
  optional `saved_peer` containing a source `peer_type` and decimal `peer_id`,
  and optional `reaction_position` preserving inline tag order for the message.
- `filter`: selected `tags` and search `text`.

The saved-message binding's peer must equal the profile's own user ID. Message
IDs must fit the positive Telegram message ID range. UUIDs and record IDs must
be unique in their scope. These cross-field checks, rejection of unnamed legacy tags without native
identity, encoded byte-size limits, and 64-bit ID bounds supplement JSON Schema.
String limits in the C++ validator count UTF-16 code units, matching Qt editors;
the schema's Unicode-character length limits are the outer structural bounds.

Version-vector keys are originating device UUIDs. Counters are integers from
1 to 2^53-1, with at most 256 device entries per record. Local writes increment
the local device's counter; deletion creates or retains a tombstone. A caller
cannot erase a record by removing it from a mutation's array. Tombstones are
not pruned by wall-clock age. They prevent old packages from recreating deleted
objects. Record ordering is not semantic; tags carry explicit `position`.

Version 2 adds native reaction identities, unnamed tags, and saved-source peers.
A native identity is `{ "kind": "emoji", "emoji": "…" }` or
`{ "kind": "custom", "document_id": "…" }`; custom IDs are nonzero uint64
strings, never JSON numbers. An empty title is valid only with a native identity.
Source peers retain their type and decimal bare ID, bounded to the Telegram
48-bit peer range, rather than a packed desktop identifier.

Native-created UUIDs use UUIDv5 under namespace
`f48d2b40-25f4-4c32-b568-af6a63b4add7`, with UTF-8 input
`environment:user_id:reaction:emoji:emoji` or
`environment:user_id:reaction:custom:document_id`. Binding UUIDs retain the
existing account/tag/message identity. New assignments append after existing
inline tags; existing positions are compacted in the same atomic mutation so
they stay bounded integers. Removal retains the old position on its tombstone;
re-adding appends again. Legacy bindings without a position use tag order with
a UUID tie-break until their message receives a new assignment. Native Saved Messages use this local
backend for every account; Telegram cloud reactions are retained separately.

The session model upgrades version 1 in one atomic mutation. Every existing
UUID, binding, tombstone, and original `emoji` field remains. Existing distinct
native identities take precedence. Legacy tags are considered in `(position,
UUID)` order: a unique valid emoji becomes its native identity; blank/duplicate
emoji tags receive a deterministic unused emoji from the native emoji palette.
Assignments are persisted once, so later ordering changes cannot change identity.
The allocator reserves other legacy emojis before choosing a fallback. If the
finite palette is exhausted, migration fails visibly without discarding records.
A replaced conflicting native identity is retained in
`previous_native_reactions`; tags are never destructively consolidated.

Known version 1 and version 2 modules merge record-by-record under version 2;
this envelope upgrade does not change record UUIDs or version-vector conflict
rules. Imported legacy records without native identities are projected on the
next session-model load. Unknown module IDs and unsupported versions remain opaque and inactive; they are exported
again without interpreting or rewriting their payload. A future version must
register its explicit migration and validation before replacing this policy.
Unknown record kinds remain stored but inactive. Unknown fields on known
objects remain part of the stored object.

The `navigation_preferences` module is version 1 and stores one account-scoped
settings record: `startup_ref`, `last_section_ref`, `restore_last_section`, and
`visible_order`. References are `all`, `local:<folder UUID>`, or
`filter:<numeric id>:<incarnation UUID>`; transient negative `FilterId` values
are never serialized. The module is additive, keeps unknown fields, and leaves
`chat_folders` as the sole source of folder membership, title, color, and rules.

## Merge and retained alternatives

Import first validates and prepares a snapshot without modifying active data.
Absent profiles, modules, and records do not imply deletion. Dominating vectors
select their newer record; concurrent edits and equal vectors with different
payloads require an explicit choice. The choices are use current, use incoming,
or keep current and archive the incoming alternative. Every resolved record
joins both vectors and increments the local counter. Resolution fingerprints
make repeated opaque/module metadata imports idempotent.

Metadata choices apply only to conflicting fields; unrelated fields from either
side remain present. Fingerprints include the competing values of conflicting
fields, excluding unrelated union growth and the four bookkeeping keys below
at any nesting depth. Other unknown names, including other names beginning with
`_telegravim_`, remain ordinary data and participate in conflict detection.

An archived alternative is retained in `_telegravim_conflict_versions` on a
record, `_telegravim_opaque_variants` in snapshot metadata, or
`_telegravim_metadata_variants` for competing metadata fields. These reserved
names and `_telegravim_resolutions` are protocol state, not user titles.
The saved-conflicts interface can list and restore alternatives. Restoration
checks the reviewed generation, archives the previous active value, and advances
the joined record vector. A deleted current value stays deleted until the user
explicitly restores an alternative. No duplicate tag UUID is invented.

Bindings to an explicitly deleted tag are tombstoned; their former active
values are archived first. A binding can be restored after its tag is restored.
Missing tags alone do not imply deletion. Duplicate active tag/message bindings
have one canonical active record, while other records become tombstones with
`_telegravim_duplicate_of` and retained previous values.

Unknown transport-level properties are retained separately from feature data:
`Snapshot.transfer`, `Profile.transfer`, and `Asset.transfer`. Export restores
them to their original manifest, account, module-entry, file-entry, or asset
registry locations. Canonical identity/path/hash fields always take precedence.
Other siblings, including archived transfer metadata variants, use the optional
`retained_transfer` object described in the
[encrypted file-transfer contract](file-transfer-format.md#unknown-metadata).

## Local storage and atomic commit

One store belongs to the local account domain. Its encryption key is derived
from the existing local domain key using OpenSSL HKDF-SHA256, salt
`telegravim-local-data-v1`, and context `encrypted-storage`. The domain key and
Telegram authorization material never enter the transfer snapshot.

Local files use AES-256-GCM with a fresh 96-bit random nonce per new encrypted
envelope. An envelope is `TGLDGCM1`, the 12-byte nonce, ciphertext, and 16-byte
tag. The header plus purpose-specific context are authenticated. JSON segments
use `segment:<sha256>`; the current manifest uses `manifest`. Asset chunk
contexts include asset SHA-256, total size, and plaintext offset. An asset file
starts with `TGLA` and contains big-endian 32-bit envelope lengths followed by
the encrypted envelopes, with at most 256 KiB of plaintext per chunk. The
streamed asset's final size and SHA-256 are checked before committing it.

JSON exists in memory while being edited or exported. Directories are owner-only
and local files are owner-readable/writable. Local writes use checked `QSaveFile`
commits with direct-write fallback disabled. Unchanged immutable segments are
reused. Known records are split into 256 buckets by UUID prefix; only changed
buckets need new files. Opaque modules have their own encrypted segments.

Cached verification records each file's native identity, size, and high-resolution
write/change timestamps. Every read of a cached snapshot and every commit checks
those attributes. Missing files fail immediately; changed files are authenticated
and hashed again. A fresh process verifies referenced assets once, with cancellation;
unchanged assets need no body read on subsequent tag edits. Snapshot data remains
bounded in memory even when resource validation scans a large asset.

An immutable generation manifest refers to every profile, module segment, and
asset. All new encrypted segments and assets are written before atomically
replacing `current.bin`. `previous.bin` retains the previous committed manifest.
An in-process mutex and an interprocess `QLockFile` serialize writers, and the
generation is rechecked before commit. Multi-profile import has one commit point.
The first access creates an encrypted empty baseline under the same lock and
atomic-write rules before any mutation can create segments. Cancelling or failing
the first mutation therefore leaves a valid empty generation available for retry.
Cancellation before that point leaves the old generation active. Failed or
cancelled preparation can leave unreferenced encrypted segments; it cannot
expose partially applied profiles. Old encrypted segments are currently retained
for recovery rather than automatically garbage-collected.

A missing, malformed, or unauthentic current manifest or segment returns a
visible error and blocks mutation. It does not delete files, clear account prefs,
or silently replace the store with an empty one. The previous manifest is retained
for recovery, but automatic fallback and a repair interface are not implemented.
An operator must preserve both manifests and referenced files before attempting
recovery; manually replacing a manifest is not an ordinary import operation.
A small owner-only `initialized` marker contains no user data. A missing current
manifest with this marker, a previous manifest, or any segment/asset is treated as
an interrupted or damaged store, including after restarting the application.

## Resource and execution boundaries

Store operations are synchronous and thread-safe; callers run them on workers.
Only a successful commit schedules the main-thread change notification. Resource
readers stream bytes from encrypted storage or an encrypted temporary quarantine.
`mutateWithAssets` commits module changes and newly ingested resources together.
Registered resource references must resolve in the snapshot's asset registry.

Limits are explicit: 128 profiles, 128 modules per profile, 1,000,000 known
records, 100,000 resources, 16 MiB per module/account document, 256 KiB per record,
256 MiB aggregate JSON, JSON depth 32, and 32 GiB of resource data. The transfer
format permits at most 116,512 entries, a 32 MiB manifest, and a 33 GiB encrypted
package. Store validation uses the same transfer-shape calculation as export;
data exceeding that representation is rejected before commit. Limits produce
errors rather than truncation. A snapshot is bounded and materialized in memory;
resource bodies are never materialized together.

Newly imported unknown modules remain inactive. Version 1 registers no message
sending executor. Any future automation module must obtain a separate, explicit
device assignment after import; imported data never authorizes sending.
