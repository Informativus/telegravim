# Same-chat E2E version 2 implementation contract

Status: implementation in progress. The version 1 prototype is not a release
candidate. A successful build alone does not establish this contract.

## User flow

The existing one-to-one chat stays in place. Each account generates its own
identity key on this device and sends only its public key. The user imports the
contact's public key and verifies its SHA-256 fingerprint through a trusted
second channel. After setup, the chat menu turns encrypted mode on or off.
Both contacts must request encrypted mode before sending encrypted text.
Pending confirmation blocks plaintext transmission and reports that the contact
has not confirmed yet. Turning the mode off affects new messages; historical
encrypted messages remain readable while their keys are present.

The full Android client is the official-source fork under `android/upstream`
with the repository's overlay and patches. A separate small companion APK does
not satisfy Android delivery. The two clients must implement the same framing,
identity checks, state transitions and receipt rules before compatibility is
claimed.

## Cryptographic context

Identity keys are P-256. A public key is exactly 65 bytes in uncompressed point
form, with the curve point, non-infinity and private-key range checks performed
by the cryptographic library. A fingerprint is the uppercase hexadecimal
SHA-256 digest of that public key. Private keys are never MTProto DC auth keys.

The shared secret is P-256 ECDH. HKDF-SHA256 uses a 32-byte zero salt and this
binary information string, in order:

1. UTF-8 `telegravim-e2e-v2`, followed by one zero byte.
2. The sender's 65-byte public key.
3. The recipient's 65-byte public key.
4. UTF-8 decimal sender Telegram user ID, a colon, and decimal recipient ID.

The resulting 32-byte key is specific to a direction and account pair.
Encryption is AES-256-GCM with a fresh random 12-byte nonce and a 16-byte tag.
This protocol does not implement a ratchet or forward secrecy. Replacing a peer
identity is the supported manual key-change operation; it is never implicit.

## Wire framing

Public-key exchange is an unauthenticated invitation:

```text
TVE2.KEY.<base64 public key>.<fingerprint>
```

It cannot confirm a mode change or automatically replace a trusted identity.
An unexpected key suspends encrypted sending until the user verifies the new
fingerprint. It never causes a fallback to plaintext.

Authenticated messages have nine dot-separated fields:

```text
TVE2.<kind>.<sender ID>.<recipient ID>.<sender fingerprint>.<recipient fingerprint>.<sequence>.<base64 nonce>.<base64 ciphertext and tag>
```

The first seven fields, joined by dots, are the UTF-8 authenticated data.
`kind` is `MSG`, `ON`, `ACK`, or `OFF`. IDs and sequence numbers are canonical
positive decimal integers no greater than the signed 64-bit maximum. Base64 is
standard, padded, unbroken and canonical. Fingerprints are 64 uppercase hex
characters. The expected sender and recipient come from the account and chat,
including the outgoing/incoming direction; wire fields cannot override them.

`MSG` plaintext is the message's UTF-8 text. `ON` plaintext is the canonical
Base64 representation of a random 16-byte handshake challenge. `ACK` plaintext
is that challenge, a colon and `1` or `0`, indicating whether the responder has
locally requested encryption. `OFF` plaintext is empty. Kind and payload must
agree; unsupported versions, invalid UTF-8 and malformed frames are rejected.

Every frame uses the next sequence for the active identity pair and direction.
The counter never wraps. Importing the same key or disabling and re-enabling
the mode does not reset it. The transmitted limit includes framing, tag,
nonce and UTF-8/Base64 expansion; text is split before encryption.

## Mode transitions

Enabling stores `requested = true`, `ready = false` and a new challenge, then
sends `ON`. Text remains blocked until an authenticated positive `ACK` echoes
that exact challenge. A negative acknowledgment leaves the request pending.

For a newly received `ON`, the recipient sends an `ACK` with its local requested
state. If its own request is pending and this is a previously unseen peer
challenge, it also retransmits its own `ON`. Remembering the peer challenge
prevents acknowledgment loops. Repeating an `ON` with the same challenge still
permits a new `ACK`, allowing recovery from an interrupted handshake. Replaying
the same accepted frame cannot trigger another response.

Disabling stores `requested = false` and `ready = false`, then sends an
authenticated `OFF`. A received `OFF` clears readiness, but does not silently
change the local user's choice to plaintext. A subsequent peer request can
complete a new handshake. State-changing frames older than the last applied
peer control sequence are rendered as historical notices without changing the
current mode. An acknowledgment for another local challenge has no effect.

## History, ordering and replay

Decryption is independent of the current sending mode. A valid historical
message can be displayed after disabling encryption or restarting the client.
Outgoing history uses the outgoing directional key, and cannot consume the
incoming receipt space.

Persist receipts for each identity pair and direction, indexed by sequence.
Each receipt records the envelope digest and its Telegram message ID. Sending
initially records a digest with no server ID; the first matching outgoing
server message binds that ID. The same sequence, digest and bound ID may be
rendered repeatedly. A changed digest or a different message ID for a bound
sequence is rejected. Previously unseen sequences may arrive out of order;
receipt uniqueness, rather than arrival order, detects duplicates.

Only a newly accepted live incoming control frame can change mode or cause a
response. Loading a history page, reloading an existing message or displaying
outgoing history has no protocol side effects. Receipts must commit before
displaying a newly accepted message or applying its state transition.

Explicit peer-key replacement disables sending until a fresh handshake. The
old public key and its receipts remain available for old history. Returning to
a previously used public key reuses its counters and receipts, preventing a
counter reset through key replacement. Lost local private keys cannot be
reconstructed from an exported settings file.

## Persistence and plaintext boundaries

Desktop identity, selected peer keys, historical key records, counters,
handshakes and receipts live in the account-local encrypted `e2e-state` file.
A separate `e2e-state-required` marker is committed before the first state write.
If state is later missing while the marker remains, sending fails closed instead
of generating a replacement identity or silently sending ordinary messages.
Atomic persistence must finish successfully before publishing an identity,
returning ciphertext, acknowledging receipt or changing the visible mode.
Unreadable state fails closed. Android stores its software EC identity, counters,
handshakes and receipts in an atomic encrypted file under `getNoBackupFilesDir()`.
A nonexportable Android Keystore AES key wraps this state; the EC private key
is present in app memory while in use. Committed bytes are read back before
the transaction succeeds. Missing state with an existing wrapping key fails closed.
Neither implementation places this data in `.tgv` or the LocalData module map.

While encryption is requested, including while confirmation is pending:

- Cloud draft saving, server link previews and inline bot queries are disabled.
- Ordinary Telegram requests do not contain reply quote plaintext.
- Control messages are selected by an internal send flag. A user typing a
  protocol-looking prefix cannot bypass encryption.
- Text encryption failure never falls back to plaintext.
- Unsupported media, forwarding, contacts, polls and inline-result sending
  fail before upload or transport, with a visible explanation.
- Scheduled sends, cloud quick replies and rich messages are unavailable in
  encrypted mode; encrypted text is sent immediately in the current chat.
- Editing an encrypted historical message remains blocked after mode changes.
- Encrypted messages display authenticated text only. Server media, rich pages,
  entities, reply quotes and bot/forward wrappers cannot supply trusted content.
- Translation, summaries and emoji interactions do not consume decrypted text,
  including after encryption is disabled. Failed sends do not create cloud drafts.

## Delivery evidence

Before release, verify two endpoints through initial setup, both confirmations,
bidirectional text, disable/re-enable, application restart and history reload.
Also verify out-of-order delivery, repeated delivery, changed ciphertext,
reflected direction, wrong account binding, old control frames, peer-key
replacement, failed persistence and absence of private state in `.tgv`.
Android evidence must come from the full fork, and its APK must be attached to
the same requested GitHub Release as the cumulative Desktop package. The
Desktop package must retain all previously accepted local features.
