# Android and Desktop feature parity

Status: independently approved design for the first privacy increment and shared
inventory constraints; implementation and runtime acceptance remain outstanding.

The Android APK must preserve the accepted Android importer and implement the
applicable local features of the coordinated Desktop release. A common version,
source revision, retained transfer archive or successful build does not establish
functional parity. Keep implementation, transfer compatibility, artifact checks
and authenticated runtime acceptance separate for every row below.

## Delivery order

Deliver one coordinated macOS DMG for user inspection first. Its packaging owner
remains unchanged. Android implementation must not delay that inspection. New
Windows and Linux builds wait for the user's DMG acceptance. Android remains a
full Telegram client in this monorepo, with its persistent package and signing
identity. Final release assets retain the publisher's common-source requirement;
later Android changes may therefore require a new coordinated source freeze.

## Source inventory

This inventory compares the current Android overlays and pinned Telegram Android
13.1.1 source with the accepted Desktop feature inventory. The privacy target is
the behavior described in [privacy-navigation-scopes](../privacy-navigation-scopes.md).
That Desktop follow-up has passed its independent review and remains separate
from the Android implementation baseline.

| Feature | Android implementation today | Work required for parity |
| --- | --- | --- |
| Encrypted settings file, link and QR | Offline importer, authenticated envelope, bounded parsing and retained archives | Preserve accepted behavior and verify transfer against final Desktop |
| Portable application settings | `send_by_enter` and `large_emoji` applied to native preferences | Every further setting needs an explicit portable mapping and real consumer |
| Same-chat E2E | TVE2 state, native menu and send/receive integration | Preserve protocol; verify two endpoints, restart, history and key-change behavior |
| Signed updates | Fork-owned catalog verification and native update presentation | Verify published feed and native download/install cycle separately |
| Import appearance | Native-style device introduction and locally bundled QR animation | Preserve accepted aspect ratio, light/dark and lifecycle behavior |
| Private reading and listening | Account policy/storage, native settings and initial controller gates | Complete topic/subpeer mapping, all receipt paths and native explicit release actions |
| Local folders and colors | Upstream cloud folders; no `chat_folders` consumer | Local native folder backend and portable identities, rules, color and order |
| Saved Messages tags | Upstream reaction/tag UI; no local `saved_tags` consumer | Local picker, naming, inline tags, filters and search without Premium API dependency |
| Local pins | No portable local-pin implementation | Native list/order backed by the Desktop module contract |
| Folder markers and navigation | No `navigation_preferences` consumer | Native markers, applicable folder navigation and stable portable references |
| Local quick replies and help | Upstream Business controller uses cloud quick-reply requests | Local native template editor, attachments, variables, preview and explicit sending |
| Local transcription and dictation | No Televim local recognizer integration | Optional local engine and resources, editable text/voice choice, no cloud fallback |
| Unified settings | Native Televim settings with offline import and account privacy settings | Native settings for remaining local feature modules |
| Physical Vim shortcuts | Desktop keyboard behavior | No imitation in touch UI; assess hardware-keyboard behavior separately |

The initial importer explicitly retains unsupported modules without activating
them. `OfflineSettingsStore.apply()` currently publishes only the two portable
booleans. Native Telegram cloud implementations do not satisfy a local-feature
row: for example, `QuickRepliesController` still calls cloud create/edit/reorder
operations. Removing a Premium check alone cannot implement a local backend.

## Shared constraints

- Preserve native Telegram Android interaction, themes and accessibility. Local
  equivalents replace storage/execution, not the native flow with a reduced tool.
- Preserve account identities, module versions, unknown fields, object UUIDs,
  conflicts, tombstones and resources from the Desktop local-data contract.
- Import, validation and local editing do not perform network requests. Normal
  messaging, explicitly requested sending and update checks retain their separate
  network boundaries. No data export or telemetry is added.
- Keep private E2E and Telegram session keys outside portable data. Account
  switching and logout must not transfer policy or pending work to another user.
- Reuse the importer's existing bounded input/storage policy. New consumers need
  explicit limits and failure behavior; silent eviction must not change privacy
  or discard user data. No full-history scan belongs on the UI thread.
- Preserve source pins and update all modified overlay/patch hashes. Do not
  alter the frozen Desktop candidate, private credentials, signing keys or feed
  trust policy as an incidental Android change.
- Use existing checks, source review, compilation and actual device observation.
  Do not author new tests, fixtures or test-only helpers without a user request.

## First increment: private reading

The existing Desktop behavior supplies the product contract. Add a native
account-local master switch and three category switches: private chats, groups
and channels. The master defaults off; the categories default selected. Bots
belong to private chats, forum topics to groups, and channel direct-message
sublists preserve their own conversation key. Do not claim native Secret Chat
parity by changing its TTL protocol implicitly; its treatment must be explicit
in review and UI documentation.

When enabled for a conversation, passive viewing and listening must not advance
local unread state or send acknowledgements. An explicit Mark as read or a
successful ordinary user send authorizes acknowledgement through a fixed server
message cutoff for that conversation. Drafts, upload start, queued/failed sends,
scheduled-message creation, history replay and E2E KEY/ON/ACK/OFF controls do not
authorize it. Encrypted TVE2 MSG is still an ordinary user message.

Receipt decisions belong in one account-owned controller. Patch native receipt
entry points before their database, notification and network effects. Recheck
queued delivery at the final native sender so enabling privacy also fences work
that was queued before the toggle. UI-only guards and an untyped global network
interceptor cannot provide this behavior by themselves.

The policy key includes account identity, dialog, topic and monoforum subpeer as
applicable. Release limits remain separate for each key. Master changes invalidate
all pending grants; category changes invalidate only that category. A callback
queued under an old generation cannot release receipts after either change.
Server updates from another client remain authoritative.

Audit all ordinary/native paths: history and discussion reads, content/listening,
mentions, reactions, poll votes, notification actions, dialog-list manual actions,
forum actions and playback. Native read methods often mutate SQLite and counters
before sending an RPC, so suppressing only the final request is insufficient.
Whole-thread mention/reaction RPCs cannot release newer messages beyond an
authorized cutoff; use exact observed content IDs or the existing safe native
equivalent. Expiring/view-once media needs explicit review of its native contract.

Successful-send integration must originate from the acknowledged local send and
carry its conversation plus generation. NotificationCenter success events alone
are insufficient unless their local-send provenance, schedule flag and E2E
control exclusion are established. Preserve ordinary cloud-send retry behavior.

Keep the unread boundary separate from the visible viewport. Down navigation in
a protected conversation should reach the latest messages, while explicit reply
return still works. Resizing, new messages and preserved unread counters must
not pull an already-bottom viewport to the first unread item.

A restart discards transient release grants and defaults to withholding further
receipts until a fresh explicit action. Policy persists account-locally; queued
receipt work cannot cross logout or account replacement. Persistence failures
must be visible and must not be reported as a successful toggle. Import remains
independent from native account initialization. Transferring this new policy
requires a separately reviewed account-scoped portable contract on both clients.

## Decisions and alternatives

| Decision | Alternatives considered | Reason |
| --- | --- | --- |
| Native backend integration per feature | Toggle Premium checks; separate management app | Preserve local execution and native behavior |
| Central account receipt policy with native seams | UI-only suppression; generic network interception | Cover local effects and queued requests without changing unrelated RPCs |
| Three independent receipt categories | Private chats only; always all chats | Match the coordinated Desktop behavior |
| Generation-bound explicit release | Release everything on send attempt; timestamp-based inference | Avoid failed-send, stale-callback and cross-thread disclosure |
| Bounded transient pending state with conservative refusal | Unbounded history cache; silent release on overflow | Preserve responsiveness and privacy under long sessions |
| Keep Desktop first | Wait for every Android port; publish old APK as equivalent | Respect immediate DMG inspection while tracking missing Android work honestly |
| Retain identical-source final publication | Re-label a held APK; weaken publisher identity check | Preserve artifact provenance and update integrity |

Subsequent module increments need their own native/transfer mapping; this first
increment does not close the remaining parity rows.

## First review decisions

The Skeptic review requested changes on nine source-backed points. All nine
objections are accepted. The following decisions resolve the design ambiguities;
implementation and runtime evidence remain required. References identify the
pinned upstream Java classes, not a claimed patched or executed implementation.

### Receipt boundaries and activation

The controller has separate history limits and one-shot content grants. A history
limit never permits listening/content acknowledgement by itself. Each explicit
release takes an immutable set of already-observed content IDs matching its
conversation and cutoff; observations arriving afterward wait for another action.
Consume a content grant once, not once per call site or duplicate success event.

The privacy switch governs future local receipt work. Set an in-memory restrictive
transition before queueing persistence; publish the selected value only after a
successful write. Serialise policy decisions and tracked-request cancellation on
the account's stage queue, and recheck at the Java transport handoff. Cancel
tracked receipt tokens on activation or relevant generation invalidation. Work
still in controller/Java queues must stop. Already-submitted transport requests
may have reached the server and cannot be retracted; do not promise otherwise.
Native transport has its own asynchronous queues, so a Java check is not proof
of cancellation inside C++. This boundary matches what an application can
actually guarantee rather than claiming an instantaneous network barrier.
The existing private-reading settings help must say: "Turning this on cannot
stop read or played receipts already handed off for sending; they may arrive
later." Product text must not expose Java/native implementation terminology.

If enabling cannot be persisted, keep the restrictive in-memory transition,
show the failure and require reconciliation; do not display successful persistent
activation. A failed disable keeps the previous restrictive policy. Before policy
load, ordinary receipt entry points wait or refuse, rather than assuming master
off. Fresh absence of a valid policy initializes the documented off default;
corruption or ambiguous publication does not. Load occurs without account/network
controller initialization in the offline importer.

### Action and identity mapping

Category follows the owning peer, in order: a user (including a bot with topics)
is private; a broadcast channel is channel; every other ordinary group is group.
The conversation key is `(environment, user_id, dialog_id, root_id, subpeer_id)`.
Ordinary discussion and forum roots populate `root_id`. Monoforum conversation
IDs populate `subpeer_id` with root zero. Saved Messages has its native self-chat
behavior. A missing chat classification is conservatively protected if either
possible category is selected; do not fetch metadata just to decide a receipt.
Every asynchronous callback carries environment, stable user ID and account
lifetime, not only a reusable account slot.

Manual authorization is attached at the actual Mark as read UI action. Capture
selected dialogs and their known maximum server IDs before asynchronous work.
For a whole-forum action, capture the parent maximum at that point and cap each
subsequently loaded topic maximum by it; no new message above the parent cutoff
is included. A folder/bulk operation uses its captured dialog set. Unknown or
unavailable cutoffs remain visibly pending instead of using `Integer.MAX_VALUE`.

`AutoMessageHeardReceiver` is currently also used for the explicit notification
Mark as read action. Give that action a distinct immutable, account-bound route
with its topic/subpeer and cutoff; generic heard/playback callbacks carry no
manual authorization. Wear/car replies wait for normal terminal send success.
The `SendMessagesHelper` pre-read after `getBotCallbackAnswer` is a bot-button
action, not an acknowledged message, and remains deferred in protected chats.

Ordinary send authorization is created in the local normal-message submission
path and tied to the original message/random ID and conversation. Exclude service
messages, quick-reply template edits, welcome/business updates, scheduled-message
creation and E2E KEY/ON/ACK/OFF controls. TVE2 MSG remains eligible. A terminal
server result with a positive message ID consumes that authorization once; quick
ACKs, storage notifications and repeated `messageReceivedByServer` events cannot
create another one. Automatic retry reuses the same authorization. Restoration
after process loss does not reconstruct consumed/transient grants from history;
manual retry or another explicit action is needed to authorize new receipt work.

Capture the category generation when terminal success is accepted, before its
deferred release callback, matching Desktop `ReadReceipts::sent()`. A send
acknowledged after a toggle can authorize the then-current policy; a release
callback queued before that toggle cannot. This is distinct from promising that
all sends submitted before a toggle are forever ineligible.

### Native integration census

| Native path | Required seam before side effects |
| --- | --- |
| `ChatActivity` visible-message history | Gate `setIsRead`, local unread counters and `firstUnreadSent` as well as the controller call |
| `ChatActivity` reactions/poll votes | Gate storage writes, message/cell flags, counters and whole-scope fallback RPCs |
| `MessagesController` history/read tasks | Gate local mutations, queued/coalesced tasks, `markDialogAsReadNow` and transport handoff |
| `messages.readSavedHistory` | Preserve parent and subpeer identity; no substitution with discussion roots |
| `MediaController`, `PopupAudioView` | Defer content without clearing native content-unread state during playback |
| `LocationController.markLiveLoactionsAsRead` | Apply policy before its throttle update and direct per-message content RPC |
| Mentions, reactions and poll votes | Observe exact IDs; release only a captured set, never whole-thread RPCs lacking a cutoff |
| Dialogs, topics, folder/bulk actions | Capture explicit action identity and cutoff before queued topic/storage work |
| Wear, car, notification and bot callbacks | Separate manual action from passive heard events and pre-success sends |
| Restored native content/delete tasks | Apply the explicit expiring-media boundary below; preserve native deletion obligations |

For a protected release, avoid native `countDiff == 0` shortcuts that zero all
unread state irrespective of cutoff. Reuse the bounded-ID storage update, obtain
the actual removed/remaining counts in the same storage transaction, and apply
its result through the revision-bound reconciliation contract below, without
blindly subtracting a stale delta or erasing later arrivals.
Do not use the native date-wide unread-push deletion for that path. Topic/subpeer
counts require the same keyed treatment. Preserve server-authoritative updates.

Use the exact-ID `messages.readMessageContents` or
`channels.readMessageContents` path already used by Desktop for observed content.
Update only those local mention/reaction/poll records. The pinned Android
whole-scope `readPollVotes` has no maximum ID or monoforum subpeer and is forbidden
as a fallback under this policy. Whether exact content reads clear poll-vote
state on the live server remains an acceptance condition; if it does not, keep
that state unread and report the limitation instead of widening the RPC scope.

### Native special media and bounds

Native Secret Chats use random IDs, dates and their own TTL protocol. They remain
outside this ordinary-chat policy, with the scope stated in the settings help.
Same-chat TVE2 is an ordinary cloud chat and remains within the policy.

In a protected ordinary chat, opening expiring/view-once media requires a native
per-open confirmation: opening reports consumption and may start the native
deletion timer. Cancel is the default; dismissal and autoplay give no consent.
Open authorizes only that exact message's native consumption effects, never
history or future content. Ordinary cloud voice/video remains protected without
this exception. The help text also explains this consequence.

Bind consent to environment, stable account, lifetime, conversation and message.
Revalidate the identity when the dialog completes and consume the decision once.
Gate every entry route before its earliest consumption effect: viewer/player
opening, delayed/download callbacks, destroyTime changes and task creation.
Changing accounts, replacing the message or closing the view invalidates the
pending confirmation. A controller-only guard after local mutation is too late.

Do not turn durable native TTL tasks into transient ordinary grants or delete
them to make a restart appear private. Stored types 11/23/101/102 and Secret Chat
tasks retain their native execution semantics within the stated exception.
Legacy persisted tasks establish already-initiated native consumption, not
consent obtained by the new confirmation. Preserve their account-bound deletion
obligations without reconstructing ordinary release grants. Review both creation
and replay paths. Previously submitted operations may
complete after a toggle or restart, as described in the transport boundary.

Initial per-account controller bounds are 1024 conversation-limit entries,
8192 retained content IDs and 512 outstanding ordinary-send authorizations.
Store IDs, cutoffs, generation/lifetime tokens and small flags only, never
message bodies, media or strong UI references. Stage/persistence work stays off
the UI thread. Overflow withholds receipts and reports that private-reading
state is full; it never disables privacy or grants a wider read. No silent
eviction may cause a duplicate success to acquire a new grant. A subsequent
explicit action can drain an existing key; otherwise restart discards transient
state while keeping only successfully saved policy. Do not suggest restarting
after a failed enable without the process-loss warning below. Content omitted because capacity was exhausted
stays unread and must be observed/authorized again. These are conservative
failure semantics, not proof of complete parity under resource exhaustion.

On restart the fresh-action rule applies only to protected ordinary categories.
Master-off and unselected categories keep native behavior after policy load.
Logout invalidates the account lifetime before clearing controller state, so
late receiver/network/storage callbacks cannot affect a replacement account.

## Second review decisions

The Constraint Guardian accepted the first review's scope and requested four
contract clarifications. All four are accepted. These decisions supplement the
first review; they do not establish implementation or runtime acceptance.

### R1: ordered durable policy

Persist one versioned master/category tuple in an account-local namespace keyed
by environment and stable user identity, independent of the reusable account
slot. Publish the whole record atomically. Retain the last successfully saved
tuple across logout/relogin of the same identity; never retain transient grants.
The offline importer neither loads accounts nor exports this policy.

Use one ordered policy writer per active account with one in-flight write and
one coalesced latest desired tuple. Every transition has a monotonic sequence and
account lifetime. Validate ownership before the durable mutation and before its
completion; serialize logout invalidation with the writer so an old task cannot
write a replacement account. A completed older write may update the known durable
baseline, but cannot overwrite a newer requested state or remove its restrictive
fence. A successful latest save publishes the selected tuple only after earlier
local commit reservations have settled.

On failed enable, protection is guaranteed only for the current process. Show
"Could not save. Protection is active now, but may be off if Android stops or
restarts the app. Retry saving before relying on it later." Retain the restrictive
fence. After process loss, load the last successfully persisted
tuple, which can still be off. Do not promise recovery of an unsaved intent.
A failed disable retains protection and shows that the change was not saved.
The next settings view reflects durable state and any current unsaved transition.
An absent first-use record uses the off default; malformed or unreadable storage
withholds ordinary receipts and reports the error. Retry uses a fresh explicit
save action. A second durable recovery journal was considered and rejected as
unnecessary complexity when the failed-save limitation is stated honestly.

### R2: local commit and reconciliation

Give each release a unique account-lifetime identity, conversation, cutoff,
captured policy generation and local reconciliation sequence. The storage queue
first obtains a commit reservation from the same serialized arbiter that handles
policy transitions. If invalidation wins, no local transaction starts. If the
reservation wins, that transaction is already committing; enabling privacy waits
for its settlement before displaying saved activation. The restrictive transport
fence is installed immediately. Do not hold the UI thread across SQLite I/O.

The transaction updates only authorized IDs/scope, computes its actual changes
and returns an explicit committed result. Exceptions roll back and produce no
success-shaped UI or notification callback. A policy toggle after reservation
does not discard reconciliation of a committed local result. Each projection
consumes its reconciliation identity once. A delta is applicable only when its
captured baseline revision still matches that projection; otherwise refresh the
affected keyed state and preserve the newer authoritative count. Never subtract
an old delta from a replacement server count. Account-lifetime checks still apply
to all storage and projection work, including logout before completion.

Bounded cached rows do not prove a complete unread total. When the baseline is
incomplete or changed, preserve the latest aggregate count until authoritative
reconciliation can establish it; update only known message flags below the
cutoff. A temporarily conservative unread badge is preferable to clearing newer
or unloaded messages. The implementation must preserve this uncertainty instead
of converting an empty local query into zero unread. Do not use date-wide push
deletion or whole-topic clearing as a fallback.

Submit ordinary receipt transport only after local commit succeeds and a fresh
generation/lifetime check passes at handoff. A cancelled/failed request may leave
local and server read state different; preserve server-authoritative updates and
never undo unrelated data. Process loss between commit and transport drops the
transient authorization. Neither restart nor failure reconstructs or expands it;
a later explicit action can reconcile again. No atomic SQLite/server rollback is
promised. Pending known storage failure is visible through native error feedback.

### R3: notification action identity

Create a distinct explicit non-exported receiver for manual Mark as read. Its
PendingIntent is immutable and has a unique random action identity; do not reuse
an identity with FLAG_UPDATE_CURRENT. Resolve that identity against a bounded
in-process registration containing the exact environment, stable account,
lifetime, conversation and cutoff captured for that displayed action. Reject
missing/unknown identities and missing account values; never default to slot zero.
No database, notification dismissal or transport effect precedes validation.

Replacing a notification cancels and retires its old registration. Logout and
process restart invalidate all old action identities; a stale action has no read
effect and asks the user to open the app. No current slot can manufacture its
former lifetime. Atomically consume a valid registration on first invocation,
capturing its exact currently observed content set up to the frozen cutoff.
Repeated delivery cannot collect more content. A newly displayed action uses a
new registration. Notification replies use ordinary successful-send provenance,
not this manual-read capability. Android immutability alone was rejected because
it does not prevent the creator from replacing extras under a reused identity.

### R4: total retained-state accounting

The 1024 conversation and 8192 content-ID capacities include pending observations,
immutable release snapshots and all in-flight work. Moving state between queues
transfers ownership of its budget reservation; it does not free capacity.
Admission reserves all required space before snapshot allocation or enqueueing.
Use at most 64 live release batches and 16 outstanding ordinary receipt request
tokens per account, with at most 100 content IDs per serialized request batch.
At most 256 notification registrations and one pending special-media confirmation
are retained per account. Snapshot copies and callback payloads count against the
same limits; native already-submitted request payloads remain counted until their
completion/cancellation is known. Native durable TTL obligations are separate.

Keep one scheduled drain per queue and bounded work within it, not one queued
runnable per observation. Coalesce projection refreshes by the bounded
conversation key. Store at most one completion payload per live batch/request.
Refuse additional work conservatively when either a count or byte bound would
be exceeded; the small-state payload budget is 2 MiB per account, excluding the
native request object's runtime overhead already constrained by token/batch caps.
No message bodies, media buffers or UI references enter this retained state.

The 512 send authorizations are created only by eligible local submission and
removed atomically on accepted terminal success or terminal failure. Automatic
retry retains the original record until it settles; unknown/retired success
events cannot recreate one. No unbounded consumed-ID history is needed. A manual
retry creates a fresh submission identity. The release cutoff is the positive
acknowledged outgoing server ID, never a newly sampled dialog maximum. Capacity
failure cannot turn a duplicate success into another admission attempt.

Retire ordinary state after definitive completion/failure/cancellation, or after
generation invalidation/logout once retained native callbacks are settled. Late
callbacks carry a detached lifetime identity and cannot touch a replacement
account; their already-accounted payload stays bounded until native ownership
ends. Receipt retries retain the original immutable scope and consume neither
new observations nor new send authorization. Conservatively dropping unsent work
is permitted; widening its scope or evicting live authorization is not.

## User-facing review decisions

The User Advocate accepted the behavior and requested two wording corrections.
Both are accepted: UA1 replaces ambiguous "session" wording with Android
process-loss consequences and qualifies restart recovery; UA2 assigns the
already-submitted receipt limitation to the existing settings help. The changes
above resolve both objections without adding a dialog or persistence mechanism.
The independent Arbiter approved the combined design with no unresolved
objections. This closes the design gate for the first privacy increment only;
later module mappings, implementation and runtime acceptance remain required.

## Acceptance evidence

Record exact source and APK provenance; then observe the native controls and
their persistence on the device. With authorized endpoint activity, compare
passive viewing/listening, successful/failed sends, manual reads, each category,
topic/subpeer separation and toggle races. Exclude E2E controls from release.
Verify navigation with unread messages retained. A source audit or APK install
alone does not prove these behaviors. Unavailable phone or peer activity remains
an explicit unverified boundary, not a passing result.


## Bot scope alignment with Desktop

Design status: APPROVED by the sequential reviews and arbiter; implementation
and runtime acceptance remain separate.

The accepted cross-client parity requirement now includes the independent Bots
scope introduced by Desktop PR129. Android must preserve the old personal-chat
selection for bots when reading an existing policy. No new user preference or
network service is required to choose the migration default.

Decisions:

- Add a fourth scope, Bots, using policy mask bit 8 and an independent generation.
  Keep the existing private/group/channel masks and generation fences. The master
  switch still governs all scopes. Avoid coupling bot changes to human-chat grants.
- Add a five-argument Selection constructor; retain the four-argument constructor
  with bots inheriting privateChats for existing callers and legacy policy reads.
- Store version 2 of the same bounded 32-byte AtomicFile record. Preserve the
  existing master bit 8 in storage; use storage bit 16 for Bots. Include the record
  version in CRC validation. Read v1 with its original CRC/allowed flags and derive
  bots from privateChats; write v2 on the next explicit save. Reject unknown versions
  or bits conservatively. Downgrades fail closed rather than misread new flags.
- Classify positive dialog IDs from the locally cached user: bot -> Bots,
  known human -> Private, absent or minimal user -> UnknownPrivateOrBot. Unknown
  identities withhold if either scope is selected and fence against both generations.
  Never fetch peer data just to classify a receipt. Secret and self-chat paths stay
  unchanged. Bot topics remain in their owning bot category.
- Expose the Bots control beside existing private/group/channel controls and save
  the full selection atomically. Existing schema/hash checks and compilation are
  used; no tests, fixtures or harnesses are authored. Device behavior remains a
  separate acceptance gate.

Rejected alternatives: keeping bots inside Private fails the accepted Desktop
contract; assigning a fresh default to existing users changes protection silently;
network peer lookup violates the local decision boundary. Runtime cost is one
cached user lookup, a fourth bit and generation, and unchanged bounded storage.


### Bot scope skeptic decisions

The skeptic returned REVISE. Both findings are accepted and are prerequisites,
not reasons to weaken the migration or privacy behavior:

- UI hydration: disable policy editing until the account policy has loaded.
  Refresh controls from that account lifetime's desired selection after load,
  and track asynchronous persistence completion/failure visibly. A full-selection
  save must not originate from constructor defaults or a previous account lifetime.
  While persistence is pending, keep the existing restrictive transition fence.
  A storage load failure shows an error and does not enable editing over an
  unknown stored policy. Navigation away stops UI observation, not the storage write.
- Classification continuity: category is policy metadata, not conversation
  identity. Equality and hashing use dialog/root/subpeer only. Track the current
  classification alongside each retained conversation within the existing bounded
  retention budget. Before a decision accepts changed metadata, invalidate all
  outstanding send/release/request/notification/consent capabilities for the
  affected conversation; no captured capability changes its meaning. Preserve
  its observed content IDs so a fresh explicit action can authorize them under
  the new category. Generation stamps remain category-specific as well as bound
  to the classification revision. Unknown-to-known and known-to-unknown changes
  both require this invalidation. Account/lifetime fences remain unchanged.
  Forget classification metadata only when no retained state references that
  conversation. On capacity failure, withhold and report the existing bounded-state
  failure rather than evict pending content or silently allow receipts.

Bot scope implementation remains held until the remaining sequential reviews
and arbiter accept these decisions. The existing native adapter does not yet wire
all explicit release actions; this increment cannot establish complete private
reading parity without finishing that already documented integration contract.

### Bot scope constraint decision

Constraint Guardian: APPROVED for design only, with no additional unresolved
objections. Classification state and detached native payloads count toward the
existing account budgets until settled. Invalidation must preserve observed IDs
that were transferred into release snapshots, not only IDs still in pending
collections. Callback observation remains account-lifetime-bound. Native release
integration, topic/subpeer identity and device acceptance are still outstanding.

### Bot scope user experience decision

User Advocate: APPROVED for design only, with no additional objection. Show
loading, pending persistence and durable success distinctly. Failed enables show
the process-loss warning; failed disables explain that protection remains active.
Provide explicit retry and show outstanding state when reopening. Load errors
keep editing disabled. English and Russian wording must mention selected enabled
scopes, preserve Secret Chat and already-handed-off receipt limitations, and not
claim the unfinished native release integration has been delivered.

Implementation order: first close the accepted hydration/save-lifecycle defect
with account-lifetime-bound controls and truthful failure feedback, then add the
Bots schema/category/generation change with stable conversation continuity. Do not
ship the separate Bots control before its backend and migration are complete.

### Bot scope arbiter disposition

APPROVED for design only. Both skeptic objections were accepted; no objections
were rejected and none remain unresolved in the design. Constraint and user
experience reviews accepted the amended decisions. Hydration/save feedback is
the first implementation increment; bot schema, classification continuity and
full native explicit-release integration still require implementation.

The hydration increment uses Telegram BaseFragment and native settings cells
inside Televim settings. It replaces the standalone Activity and removes its
duplicate top-level settings entry. Loading and saving are observed only while
the screen is resumed, without cancelling storage work on navigation. The save
entry point requires the captured account lifetime and a successfully loaded
policy. Each toggle starts from the latest desired selection, preserving other
fields. Bot backend/control and schema migration are not part of this increment.


Hydration implementation review found two feedback defects; both are addressed.
Storage failure is now retained independently of transient capacity failures,
so they cannot hide an outstanding save error or its retry action. Failure text
without new unsaved protection refers to the previously saved protection
settings, without claiming that protection is enabled when master was off.
Compilation and real-device lifecycle behavior remain separate acceptance gates.


The Bots storage/controller/UI increment passed independent source review.
Version 1 retains its original CRC and rejects flags outside its original mask;
Bots inherits the stored personal-chat selection without writing during load.
Version 2 saves all five fields, retaining the master storage bit and assigning
Bots its own bit. Cached absent/minimal users select the unknown human-or-bot
category without a fetch. The native Bots row saves the whole loaded tuple.
Policy continuity, compilation and device behavior are separate checks.


The Bots policy increment keeps conversation identity independent of category.
Retained keys carry a classification revision; changed cached classification
invalidates captured capabilities without discarding observed content. Invalid
release snapshots retain their IDs until a fresh explicit action recovers them.
Recovery admits the complete bounded result before changing any cutoff or IDs,
and counts snapshots plus outstanding request payloads until their owners finish.

Native adapters must call `decision` with freshly cache-resolved metadata before
using an asynchronous capability. Capability validation does not adopt metadata
from a captured old conversation. Notification/consent category mismatches revoke
captured state without rolling classification backward. Pending sends retain only
the classification revision: preference toggles preserve their eligibility, while
terminal success captures the current selection generations in the new release.
The coordinator caught and corrected an initial use of submission-time selection
generations, preserving the previously accepted send-through-toggle behavior.

These changes do not wire native explicit releases or supply missing topic/subpeer
identities. Those implementation and authenticated-device gates remain open.


History receipt gates now preserve the native conversation identity both at
`markDialogAsRead` entry and for the queued `ReadTask`: ordinary forum/bot roots
use rootId, and monoforum peers use subpeerId with rootId zero. The native
monoforum classification is captured once and reused by the queued task. Content
adapter overloads accept the same explicit identity, but existing content call
sites still need their message/topic mapping and complete native release wiring.

Missing dialog IDs no longer become Saved Messages. Unknown or invalid ordinary
conversation identity uses the effective union of selected privacy scopes,
including restrictive unsaved transitions; unloaded policy withholds. Actual
Saved Messages and Secret Chats retain their native behavior. Independent review
found no blockers, the new patch applies exactly to pinned prepared source, and
the policy compiles for Java 8. Full Android compilation/device acceptance remain
separate from these source checks.


Account teardown now invalidates the policy synchronously at logout entry and
before clearing UserConfig identity. Initial configuration load publishes the
first lifetime; only successful authentication can replace an inactive lifetime.
Late profile updates cannot reopen it. Native adapters can retain an immutable
AccountHandle across queues, but isCurrent is only an ownership snapshot, not an
atomic storage or transport authorization. Queued native read tasks and callbacks
still require migration to the captured owner.

Checked SQLite transactions require SQLITE_DONE for BEGIN IMMEDIATE, COMMIT,
ROLLBACK and DML stepping. Failed preparation or checked stepping prevents commit;
failed BEGIN or ROLLBACK poisons the wrapper until database replacement. Closing
checked work rolls back instead of committing. The release adapter must execute
one uninterrupted storageQueue operation against the pinned database, roll back
all failures and dispose every handle before teardown. These primitives alone do
not implement native read release or establish live database-error behavior.

Pending-send authorization can now begin while privacy is disabled, so enabling
privacy before terminal success preserves the accepted send-through-toggle
contract. A committed local result remains eligible for lifetime-bound projection
reconciliation after a policy toggle; that toggle still fences unsent transport.
Unknown or changed aggregate baselines require refresh rather than guessed unread
counter subtraction. Native release integration and device acceptance remain open.
