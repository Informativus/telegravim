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

## Encrypted file, link and QR

The module is placed in one profile of the existing [version 1 `.tgv`
container](file-transfer-format.md). The profile identifies the selected Desktop
account by Telegram environment and stable decimal user ID. Its existing local
profile UUID is reused when present. The source device UUID is retained; the
export receives a fresh generation and package ID. The export contains no other
profiles, module data, resources, opaque metadata, credentials, Telegram sessions,
device activation or E2E private keys.

The entire existing `TGVXFR01` envelope is encoded into this URI:

```text
televim://settings/v1#<base64url-of-the-complete-encrypted-tgv-file>
```

Base64url uses the URL-safe alphabet without padding. The payload includes the
magic, iteration count, salt, nonce, ciphertext and authentication tag. It is not
a file URL, server address or pairing token. No part of the link is fetched from
the network and unsupported input must not fall back to Telegram URL resolution.

The encryption format is unchanged: AES-256-GCM with the full header as
AAD; PBKDF2-HMAC-SHA256 with exactly 600000 iterations; exact UTF-8 password bytes
without normalization. Settings export accepts any nonempty password, up to 1024 UTF-8 bytes,
and requires matching confirmation. Full local-data export retains its minimum
password length. The export password is entered separately on
Android and is never included in the link, QR or clipboard. Clipboard copying
occurs only when the user selects **Copy link**.

The complete ASCII URI is limited to 8192 bytes. One QR is limited to 2900 bytes
of that complete URI and uses Low error correction with a four-module quiet
zone. Rendering fits the actual row width, keeps integer device pixels per module,
and retains all four quiet-zone margins, with no logo or center cutout. The
complete link is also visible in a read-only field.
An oversized QR or link is rejected without truncation; the encrypted `.tgv`
file remains available. Format capacity is not proof of camera readability:
the dense QR must also be checked on a physical screen and phone.

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
