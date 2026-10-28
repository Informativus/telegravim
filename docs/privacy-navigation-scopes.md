# Private reading and navigation

Private reading must not pull the viewport back to the first unread message.
The unread boundary belongs to receipt delivery; an explicit navigation action
must preserve its destination even while that boundary remains unchanged.

## Behavior

- Keep the existing account-level private-reading switch and add independent
  category switches for private chats, groups and channels in Telegravim Privacy.
- All categories default to selected, preserving the previous all-chat behavior.
  The existing master switch keeps its saved value. No migration sends receipts.
- In selected categories, viewing and playing messages defer acknowledgements.
  Successful user sends and explicit Mark as read retain their existing cutoff.
  Unselected categories follow normal Telegram behavior.
- In private-reading conversations, the down button and its Ctrl+G binding go to
  the latest messages while preserving explicit reply-return navigation. Resizing
  or receiving messages must not move a viewport at the bottom to an old unread
  boundary. Initial opening may still show the first unread message.
- Preview, scheduled-message and unrelated list behavior remain unchanged.

## Decisions and assumptions

Three independent category switches were selected over a fixed private-only
preset because they support either combination requested by the user. Per-chat
exceptions are outside this change. Bots use the private-chat category; group
topics use the group category. Channel direct-message sublists follow their
owning peer classification and retain separate acknowledgement cutoffs.

Use account-local key-value preferences and constant-time peer classification;
do not scan history, rewrite positional serialization or add network requests.
Settings survive restart, and server updates from other clients remain
authoritative. Existing deferred receipts are invalidated conservatively on
configuration changes; enabling privacy must never release them.

Verification uses existing relevant checks, source review, compilation and
actual UI observation when available. No new tests are authored. Source review,
artifact integrity and authenticated behavior are separate evidence layers.
The frozen 106 builds remain intact; this follow-up needs a new reviewed version
and cannot be claimed included in a 106 artifact.
