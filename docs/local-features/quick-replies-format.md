# Local quick replies

The native shortcut editor uses the account-local message backend. Media
preparation, preview, and persistence do not upload files or call cloud template
APIs. Ordinary chat insertion retains an ordered snapshot for explicit Send.
These are source contracts; integrated application and portable-transfer checks
remain required before LF-16 acceptance.

The account-scoped `quick_replies` module uses schema version 1. Its records
participate in the shared local-data version vectors, tombstones, encrypted
storage, merge review, and portable asset transfer.

A live `kind: "reply"` record contains:

| Field | Meaning |
| --- | --- |
| `id` | Stable lowercase UUID. |
| `title`, `category` | User-facing strings, at most 128 UTF-16 code units each. |
| `shortcut` | 1–32 characters in Telegram’s native shortcut alphabet; unique after case folding within the account. |
| `text` | Template text, at most 32768 UTF-16 code units. |
| `entities` | Formatting objects with string `type`, UTF-16 `offset`/`length`, and string `data`. |
| `variables` | Objects with `name` and `default` strings. The template refers to a variable as `{{name}}`. |
| `attachments` | At most ten objects with `sha256`, decimal-string byte `size`, basename `filename`, and `mime`. |
| `position` | Nonnegative ordering index; UUID breaks ties. |

Supported entity names are `bold`, `italic`, `underline`, `strike`, `code`,
`pre`, `blockquote`, `spoiler`, `link`, `mention`, and `custom_emoji`.
The desktop editor preserves unknown record/module fields, unknown record kinds,
and unknown entity fields. It retains unknown entity types when editing metadata
or supported formatting; changing text with unknown entity types is refused so
that offsets and unsupported formatting are not silently discarded.

Files use the shared content-addressed encrypted asset store. Portable records
contain hashes and basenames, never source paths. Temporary decrypted copies
have owner-only permissions, are reused for identical attachment lists during
the session, and are held by typed shared leases. Closing an editor or stopping the model
drops its references; a background persistence job, asset reader, preview, or
insertion snapshot can retain the lease until it finishes.
The ordinary Telegram file confirmation and upload flow owns the final sending
decision. Cancellation never sends a template.

Settings → Telegram Business → Quick Replies opens the native replies section
without Premium. Search matches titles, shortcuts, categories, and text. The
editor supports rich text, attachments, category assignment, and reordering.
Typing `/` in a chat lists local templates alongside ordinary commands. A local
shortcut wins a name collision; explicit `@bot` commands remain available.
Selecting a local row by Enter, Tab, or mouse opens a preview with variable
fields. Inserting replaces only the slash query and preserves the rest of the
rich draft. If the draft changes while previewing or loading files, insertion
is refused. Text stays in the composer; attachments open the native file
confirmation. These paths do not invoke the cloud quick-reply send APIs.

Variable expansion applies once to the original template. Inserted values are
plain text, are never recursively expanded, and inherit formatting ranges that
cover the placeholder. The preview uses the same expansion and formatting as
the inserted message. Editing a reply that changed or was deleted since the
editor opened is refused instead of restoring stale data.

Verification for this source change includes isolated language generation,
JSON parsing, and native compiler syntax checks. Runtime creation, persistence,
formatted insertion, file confirmation/cancellation, restart, and portable
export/import still require the integrated macOS application check.

The native-backend continuation adds independent `kind: "message"` records.
Each has its own UUID, version vector, tombstone, `reply_ref` UUID, `position`,
`text`, `entities`, `attachments`, and a forward-compatible `media` object with
a string `kind`. Optional `group_ref` and `reply_to_ref` contain UUIDs, never
runtime message IDs. Template deletion tombstones all its current message
records in the same transaction. Legacy single-body records are converted on
the first message edit without losing their content.

Concurrent deletion of a template and addition of a message can retain a live
child under the known parent tombstone. These records remain in storage and
asset export. They must be surfaced by the native settings recovery list,
where the user explicitly restores the parent or deletes the retained children;
there is no automatic parent resurrection. The backend exposes this list and
explicit recovery/deletion mutations. Native recovery remains an explicit user action.

Editor drafts use a deterministic UUID derived from the parent reply UUID and
`kind: "editor_draft"`. The `reply_ref` identifies the template. Independent
`normal` and `edit` objects retain rich text tags, cursor/selection/scroll,
reply quote and offset, and portable link-preview preferences. Their
`message_ref` is the message UUID; no numeric shortcut alias, runtime message
ID, draft-key number, or cached webpage ID is serialized. An edit additionally
stores `source_version_vector`, the original message revision captured when the
edit began. An unmigrated legacy message instead stores
`source_legacy_parent_version_vector`, its original parent revision. These
optional, mutually exclusive snapshots use the existing version-vector format
(nonempty UUID-to-counter map, at most 256 entries). Missing/null baselines from
older clients, changed revisions, deleted targets, and unsupported source media
retain the draft as recoverable text; they never resume editing the latest source
implicitly. Recovery requires confirmation to save the text as a new message;
editing an existing source requires a fresh explicit edit. Parent deletion
atomically tombstones the editor draft along with its messages.

The native compose controls have an explicit local template context and keep
normal/edit state separate from history draft keys. Local drafts do not
register normal draft sources, save cloud drafts, emit typing actions, or
consume/change the ordinary chat's forward draft. The view resolves UUIDs to
session-only message IDs when restoring a draft. A deleted edit target keeps
its draft text for explicit recovery; saving that text as a new template
message requires confirmation. These state paths are connected to the local native shortcut section.

Editor-draft writes compare the expected record version before replacing known
fields. Source baseline maps are replaced as complete values, never recursively
merged; omission/null removes the old snapshot. Unknown draft object fields
survive supported edits. An active edit whose source disappears is captured into
in-memory deferred recovery before cancellation, including its frozen baseline,
while the independent normal draft remains intact. A session-owned
writer keeps the latest pending snapshot while one asynchronous write is in
flight, confirms content and revision only after success, and can finish a
queued snapshot after the editor closes. Failed writes retain the previous
confirmation and report the storage error; they are not silently marked saved.
Deleted-target recovery checks its draft snapshot again before removing the
retained edit state.

## Message assets and native previews

A message may reference up to **8192 asset descriptors** in `attachments`.
The legacy parent `reply.attachments` limit remains ten. The message limit counts
all retained originals and derived thumbnails/covers, not just visible messages
or album members; native rich pages still obey their independent native limits
(50 media, 500 blocks, depth 16). Exceeding a bound fails the whole mutation.
No asset is silently dropped, compacted, or reindexed to make room.

Media role values are zero-based integer indices in that message's attachment
array. `asset` selects the current primary photo/document; `thumbnail_asset`,
`good_thumbnail_asset`, `video_thumbnail_asset`, and `video_asset` select derived
content. A video cover uses `cover_asset` plus its complete `cover_asset_media`
photo descriptor. Composite game/invoice previews similarly use `photo_asset`
and `document_asset` with sibling `_media` descriptors. File names, MIME types,
and byte sizes come from the selected descriptor, never array element zero.

`media.kind: "rich_message"` stores the codec page at `media.rich`. Its native
photo/document indices resolve through sibling `media.rich_assets` entries
`{ "asset": index, "media": descriptor }`; there is no nested retained envelope.
Unknown rich versions, blocks, fields, or media are retained for transfer and
reported per message. Destructive edits of unsupported content are refused.

Optional `media.attached_sticker_sets` augments the unchanged `attached_stickers`
document-ID array. It is an object keyed by those canonical positive uint64
IDs, with at most 1024 entries. Each hint contains an optional `id` (set ID in
the same decimal format) and/or `short_name` (1–64 ASCII letters, digits, or
underscores); at least one usable field is required. Hints contain no access
hashes or credentials. The same contract applies recursively to cover,
photo/document and rich-asset media descriptors. Storage validates known shape,
ID bounds and membership while retaining unknown hint fields for transfer;
supported editing refuses unknown fields. Missing hints and legacy ID arrays
remain intact. Capture uses already known session metadata, and explicit Send
resolves native sticker context in the current account; a stored hint does not
claim offline replay or authorize a network action during editing.

Prepared edits append new assets after existing attachments. Private source
paths and leases exist only in memory and are never serialized. Each background
save job and each asset reader independently captures the preparation lease.
Readers stream bounded chunks into one atomic asset-and-record mutation while
preserving account, parent, version-vector, and legacy-revision checks.
Materialization verifies sizes and SHA-256 while streaming to owner-only files;
its typed lease survives model cache clearing and must be retained by every
consumer that keeps paths.

The view retains a transient HistoryItem per source message UUID, applies edits
in place, and preserves source ordering independently of runtime numeric IDs.
Group/reply UUIDs map to native session-only group/message IDs. Preparation
callbacks are fenced by per-message operation tokens and revisions; failed or
unsupported preparations leave source records and drafts intact and report the
message UUID. Preview photos and video thumbnails use a 128 MiB working-content
budget per record; exceeding it is an explicit preview error, not media removal.
Image decoders may have additional internal allocation beyond that budget.

Legacy parent attachments expand into one deterministic message UUID per file,
with text/entities on the first message and all file descriptors preserved.
The first edit or deletion migrates the complete legacy body atomically; deleting
one migrated file does not delete its siblings. Unknown parent metadata remains
on the parent record.

Native previews preserve document/audio/voice/round-video/sticker/animation
metadata, spoiler and caption placement, covers, contacts, geo/venue/live-geo
semantics, rich content, and inline keyboard data. Inline game/invoice metadata
and local preview assets remain portable, but server-bound replay, payment,
callback/web-app actions, live-location renewal, sticker-set association, and
link-preview resolution require their native server context. Storing or
previewing a record does not execute those actions. Replay limitations remain
explicit metadata and are surfaced per record; a preview is not offline replay.

Validation evidence must distinguish compiler syntax from link, application
interaction, restart, archive round-trip, and Android behavior. No runtime or
cross-client acceptance follows from these source contracts alone.
