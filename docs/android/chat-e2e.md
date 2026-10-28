# Same-chat encrypted text

Implementation in progress. This task adds TVE2 to the full Android client;
Telegram Secret Chats remain separate and are not used as a substitute.

Open the existing one-to-one chat menu and choose **Chat encryption**. Exchange
public keys, compare the fingerprint through a trusted second channel, and
confirm it in **Encryption keys**. Both contacts enable encryption in the same
conversation. Pending confirmation blocks plaintext. The same menu turns the
mode off for new messages; encrypted history remains readable on this device.

Only text is supported. Media, forwarding, editing encrypted messages, inline
bots, scheduling and cloud quick replies are unavailable in encrypted mode.
Cloud drafts, automatic link previews, automatic translation and animated emoji
lookups are suppressed for protected content. Plaintext stays in message display
objects; Telegram message storage retains the encrypted wire text.

The [TVE2 contract](../local-features/chat-e2e-v2.md) specifies the framing,
account and direction binding, handshake, replay receipts and key replacement.
Android uses P-256 ECDH and AES-256-GCM. Its software identity and all protocol
state are encrypted with an account-specific nonexportable Android Keystore key
under the app no-backup directory. Offline settings import/export does not read
or write this state. This implementation has no ratchet or forward secrecy.

Source integration is carried by numbered patches against the pinned upstream
and the reviewed Java/resource overlay. Java/resources, signed APK, Desktop
interoperability and the runtime cases in the contract must pass before release.
A successful baseline APK from PR #96 does not validate these E2E additions.
