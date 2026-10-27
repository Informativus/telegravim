# Local voice dictation

The local transcription settings include an independent, account-specific
switch for recording with transcription. It is off by default. When enabled,
a short click cycles the composer button through voice, video, and text;
holding the text button records audio. Without a camera, the cycle contains
voice and text. Turning the switch off restores the native two-mode cycle.

Releasing a recording in text mode opens a review dialog with audio playback,
an editable transcript, and explicit choices to send text or voice. Closing
the dialog sends nothing. Ordinary voice and round-video recording retain
their existing behavior. Paused recordings can still be resumed or trimmed
before review.

Recognition uses the existing local model and selected recognition language.
The recording joins the session's recognition queue, so incoming transcription
and dictation do not load competing model instances. Work runs off the UI
thread, exposes progress, and is cancelled when its review is dismissed.
Recognition failure leaves voice playback and voice sending available.
Missing models use the existing model download flow; recorded audio is never
uploaded for recognition. Final sends retain native chat restrictions,
payment checks, scheduling, reply context, and private-reading behavior.

The agreed interaction is a third button mode. Alternatives considered were
opening the review after every voice recording and a multi-click gesture;
the explicit mode preserves normal recording and makes the chosen action
visible. The settings switch makes the additional mode optional. This is a
local extension of the native composer, maintained with the existing local
transcription backend; it does not depend on another dictation application.

Verification uses existing Vim and transcription checks, a macOS Release
build, signing and DMG verification, then interactive checks of Command-comma,
mode switching, editable recognition, playback, cancellation, and the two
explicit send choices. No real messages are sent automatically during checks.
