# Initial Android app icon

The supported Televim Standalone APK uses an original V monogram. The current
Desktop resources still contain Telegram's original paper-plane logo, so this
Android mark is not presented as an existing Desktop asset.

The default icon is a white V on a green-to-teal gradient. Its 108-unit native
vector keeps the complete mark inside the adaptive icon's 66-unit safe circle.
All six existing launcher aliases use the same foreground and monochrome mark.
The alternate icon backgrounds, names and selection behavior remain unchanged;
their settings previews and change notifications also use the V. Android 12's
launch splash uses the same path in teal.

The source lives in `android/overlay/TMessagesProj/src/main/res/` and
`android/overlay/TMessagesProj_AppStandalone/src/main/res/`. Content hashes in
`android/source.json` bind it to the prepared source. The supported minimum is
Android API 26, where the adaptive resources override the upstream density
bitmaps. Upstream assets and their licenses remain intact in `android/upstream`.

Native XML was chosen over copied PNG sets or a new raster design: it stays
small, scales without raster artifacts, needs no runtime dependencies or
network access, and can be maintained alongside the other Android overlays.
This is a bounded initial app-icon change, not a redesign of Telegram's in-app
illustrations or the Desktop identity.
