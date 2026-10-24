# Local pinned items, version 1

`pinned_items` is registered by the common store before any session or feature
widget is constructed. It uses the encrypted store and `.tgv` transfer contract,
including account mapping, atomic writes, version vectors, explicit import
conflicts, retained conflict versions and unknown metadata.

The Desktop model stores local pin membership and order separately from the
server state. Its input and output peer IDs are in-memory values only. JSON uses
`peer_type` (`user`, `chat`, `channel`) and canonical positive decimal `peer_id`
strings. A reference belongs to the enclosing production/test Telegram account.

## Scopes

| Scope | Meaning |
| --- | --- |
| `main` | Main chat list |
| `archive` | Archived chat list |
| `saved` | Sections of the account's Saved Messages |
| `filter:<filter_id>:<scope_id>` | Existing cloud folder and its known lifetime |
| `local:<folder_uuid>` | Reserved for LF-03 local folders |

Cloud filter IDs are canonical positive decimal strings up to 2147483647.
UUIDs are lowercase without braces and must be nonzero. Peer IDs are limited to
the actual 48-bit Telegram peer identity range. Runtime validation checks these
bounds and semantic uniqueness in addition to the structural JSON Schema.

## Record identity and versions

Every record has `id`, `kind`, `deleted` and the common `version_vector`.
The store assigns and advances vectors; wall-clock time never resolves conflicts.
Known record IDs are deterministic UUIDv5 values in namespace
`19fa5d14-f51b-4fe8-b4cd-43bbfce55628`. The UTF-8 name is:

```text
<environment>:<user_id>:pinned_items:<key>
```

| Kind | Key | Additional fields |
| --- | --- | --- |
| `pin` | `pin:<scope>:<peer_type>:<peer_id>` | `scope`, `peer` |
| `order` | `order:<scope>` | `scope`, ordered `peers` array |
| `filter` | `filter:<filter_id>` | `filter_id`, `scope_id` |

Validation rejects known records whose UUID does not match their account and
semantic identity. Creating the same pin on another device therefore edits the
same record, and a repeated import cannot create a second copy. Independent
concurrent edits are exposed by the existing import conflict preview. Choosing
both keeps one active version and archives the other; it does not duplicate the
pin. Unknown record kinds remain opaque and are retained without being executed.

## Membership and order

A `pin` record with `deleted: false` explicitly pins that peer locally. It remains
pinned even if the cloud pin disappears. A `pin` record with `deleted: true`
is a durable local unpin override: it suppresses the cloud pin as well. The peer
and scope fields remain present on tombstones so removal survives transfer.
No local membership record means the peer follows current cloud membership.

An `order` record stores preference order, including cloud peers. An order-only
reference does not pin a peer that is absent from the cloud and has no active
local pin. The visible sequence is computed as follows:

1. Begin with current cloud members.
2. Add explicit local pins and remove explicit local unpins.
3. Emit active peers in retained local order, then remaining cloud order.
4. Append any remaining explicit local pins in stable peer order.

New pins are placed first. Dragging persists order without turning cloud peers
into explicit local pins. A native list may temporarily omit unloaded or
ineligible peers; saving its visible order retains omitted references and their
relative order after the explicitly ordered peers. Unpinning removes the peer
from the active order but preserves its membership tombstone. Extra metadata on
removed order peer references is retained on that tombstone in
`retained_order_peers`; normal field updates preserve unknown record and peer
fields. Reordering preserves unknown metadata on every retained peer reference.

Unpinning prunes only the existing stored order; it does not create an order,
restore a deleted order or copy the visible list into it. Additional cloud pins
therefore do not block removal when the local order limit is reached.

Deleting a conversation writes durable unpin overrides for the main list,
archive and every known cloud-folder lifetime in one transaction, including
retired scopes and unresolved peer references. It also removes that peer from
their stored orders. Saved Messages sections and local-folder scopes are not
changed. Clearing message history alone does not remove local pins.

Each scope permits up to 4096 active local pins and 4096 order references. These
are resource bounds, not Premium/cloud limits. A write that would exceed them
fails visibly and keeps the committed state; the model never silently trims
pins. The shared 256 KiB per-record, 16 MiB per-module and total JSON bounds also
apply. No assets or credentials are introduced by this module.

## Cloud folder lifetime

A `filter` record maps a cloud filter ID to the lifetime UUID used in its scope.
The first observed incarnation is UUIDv5 using the same namespace and account
prefix with key `incarnation:<filter_id>`, allowing the initial cloud scope to
match across devices. Only a complete, authoritative cloud-folder list may be
used to synchronize lifetimes.

When a known folder disappears, its mapping becomes a tombstone; the old scope's
pins and order remain retained. If that ID is observed again, a fresh random UUID
is assigned. It cannot silently reuse the observed old scope. The lifetime
mapping itself has a version vector and participates in import conflict review.
Local pin writes verify that their filter incarnation is still active when the
serialized mutation runs.

Telegram supplies a numeric filter ID, not a globally unique server generation.
If deletion and reuse both happen while every observing client is offline, the
protocol cannot distinguish them using that ID alone. This implementation does
not claim to detect such unobserved server lifetimes. Local-folder UUID scopes
avoid that ambiguity and will be connected by LF-03.

## Persistence and failure behavior

The session model reads and mutates the store on workers, serializes pending
mutations, and updates the native UI optimistically. After each write it reads
back committed state and reapplies remaining pending changes. A failed write
rolls back that operation, exposes the error and preserves all other committed
records. Import/store notifications trigger a reload after queued changes finish.
`busy()` includes pending writes so the owning session can defer application exit
until their completion.

This format provides the Desktop portion of LF-18. Real Android restoration,
QR transfer and LF-03 folder integration require their own implementation and
acceptance; none is proved by schema compatibility or Desktop compilation.
