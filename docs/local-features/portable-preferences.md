# Portable application preferences, version 1

The Desktop action **Settings → Televim Settings → Export settings to Android** exports an explicit selection of two application settings.
It does not export the binary Desktop settings stream or the generic preference
registry. Existing full `.tgv` file transfer remains available separately.

The module is an application-settings snapshot:

```json
{
  "module_id": "portable_preferences",
  "schema_version": 1,
  "scope": "application",
  "values": {
    "send_by_enter": true,
    "large_emoji": false
  },
  "records": []
}
```

`values` contains a nonempty subset of the two keys above. Each present value
must be a JSON boolean; a missing key means leave that setting unchanged.
`records` is an empty array: these settings are not account-owned entity records
and do not carry record UUIDs or version vectors. Receivers must validate the
literal module ID, integer version, application scope and exact key allowlist.
Raw JSON import accepts this single module document and rejects additional
top-level fields, additional value keys and nonempty records.

| Portable key | Desktop source | Android destination |
| --- | --- | --- |
| `send_by_enter` | `Core::Settings::sendSubmitWay()` enables plain Enter (`Enter` or `Both`) | Global `mainconfig.send_by_enter` |
| `large_emoji` | `Core::Settings::largeEmoji()` | Global `mainconfig.allowBigEmoji` |

The first field carries only the boolean behavior of plain Enter. It does not
transfer the complete Desktop keyboard map or the separate Ctrl/Cmd+Enter
behavior. Both destination settings affect the Android application across all
accounts. An account profile must not select another preferences file. Conflicting
global values in multiple imported profiles must not be resolved by traversal
order. Import previews must distinguish settings that will actually be applied
from opaque local-feature modules that the receiver can only preserve.

## Encrypted link, QR and file

Desktop encrypts only the selected module above. It contains no account
profile, device identifiers, credentials, Telegram sessions or E2E keys. The
compact `TGVSET01` envelope has the same 40-byte header layout as the `.tgv`
container: eight magic bytes, the big-endian iteration count, a 16-byte salt and
a 12-byte nonce. The ciphertext is the compact JSON module, followed by the
16-byte GCM tag. Encryption uses AES-256-GCM with the full header as AAD and
PBKDF2-HMAC-SHA256 with exactly 600000 iterations over the exact UTF-8 password
bytes, without normalization. The decrypted module is limited to 64 KiB.

```text
televim://settings/v2#<base64url-of-the-TGVSET01-envelope>
```

Base64url uses the URL-safe alphabet without padding. The link for both
settings is about 300 characters, so its QR stays small enough for a phone
camera to read from a screen. No part of the link is fetched from the network
and unsupported input must not fall back to Telegram URL resolution. Android
still accepts `televim://settings/v1#` links containing a complete `.tgv`
envelope.

Settings export accepts any nonempty password, up to 1024 UTF-8 bytes, and
requires matching confirmation. Full local-data export retains its minimum
password length. The export password is entered separately on Android and is
never included in the link, QR or clipboard. Clipboard copying occurs only when
the user selects **Copy link**. **Save encrypted file** writes the same
`TGVSET01` envelope for Android's local file route.

The complete ASCII URI is limited to 8192 bytes. One QR is limited to 2900 bytes
of that complete URI and uses Low error correction with a four-module quiet
zone. Rendering keeps integer device pixels per module and all four quiet-zone
margins, with no logo or center cutout. An oversized QR or link is rejected
without truncation.

The Android receiver has its own smaller documented resource limits, including
an 8 MiB encrypted file cap. This small settings export fits within that cap;
general Desktop archives may exceed it and must be rejected explicitly rather
than partially imported.

## Plain JSON option

**Save JSON without password** writes exactly the selected module above, without the
account profile or device identifiers. The UI explicitly says that this file is
not encrypted and requires no password. Its allowlist prevents private keys,
tokens, proxy credentials, message data or arbitrary executable settings from
entering this path.

Export only reads the selected settings and local identity metadata. It does not
change the Desktop settings or persist a new module in its local-data store.
Existing Desktop importers may retain `portable_preferences` as an unsupported
opaque module; that does not mean they apply its global settings.
