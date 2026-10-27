# Same-chat E2E text mode, version 1

Unreleased prototype. Its state notices, direction binding and history/replay
handling do not meet the release requirements. The replacement contract is
[version 2](chat-e2e-v2.md); this page records the original wire format only.

Telegravim's local E2E mode keeps one ordinary user chat and changes how new
text messages are sent. It is separate from Telegram Native Secret Chat: the
server still carries an ordinary message, but its text is an authenticated
`TVE1` envelope. The chat menu enables or disables the mode without creating a
second conversation.

## Setup and key identity

Each client creates one P-256 identity key pair on first use. The public key is
an uncompressed 65-byte point (`04 || X || Y`) encoded with standard Base64.
The fingerprint is the uppercase SHA-256 digest of those 65 bytes. Users send
the public key in the chat, exchange it through a second trusted channel, and
compare fingerprints before enabling the mode. A stored peer key cannot be
replaced implicitly; an explicit replacement clears the mode and both sequence
counters.

The Desktop private key and peer state are written atomically to an encrypted
account-local `e2e-state` file, outside the LocalData Snapshot module registry.
The client commits this file before exposing a newly generated public key or
sending an encrypted message. It migrates the earlier account preference records
only after the new file is committed, and refuses to replace unreadable identity
data with a newly generated key. Android keeps its private key in
Android Keystore and stores only the peer key, mode and counters in private app
preferences. Neither platform exports an identity or session key in `.tgv`.

## Message format

The shared secret is P-256 ECDH. HKDF-SHA256 derives the AES-256-GCM key from
the shared secret and the fixed context `televim-e2e-v1` followed by the two
public keys in unsigned lexicographic order. A text message is encoded as:

```text
TVE1.<sequence>.<base64 nonce>.<base64 ciphertext and tag>
```

The authenticated data is the UTF-8 string `TVE1.<sequence>`. Nonces are fresh
12-byte random values and the GCM tag is 16 bytes. Each direction has a
persisted counter. A receiver accepts only a sequence strictly greater than
the last accepted one, so replays are rejected and out-of-order delivery is
not silently reordered.

Key and state notices use `TVE1.KEY.<public key>.<fingerprint>` and
`TVE1.STATE.ON|OFF.<fingerprint>`. They remain ordinary chat messages but are
rendered as notices. While the mode is enabled, plaintext media and text edits
are refused; disabling it affects only new messages. Existing history is never
rewritten.

While the mode is enabled, draft text stays local and the composer does not
request server-side link previews or inline bot results. Reply quote text is
not included in the ordinary Telegram request. Messages are split before
encryption with room for UTF-8 and Base64 expansion, nonce, tag and framing.

This version intentionally requires an out-of-band fingerprint comparison and
does not claim protection from a man-in-the-middle attack when users skip that
step. It also has no ratchet or forward secrecy; a future wire version must
change the prefix and negotiate a new state rather than reinterpret `TVE1`.
