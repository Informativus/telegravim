# Local voice transcription

Accounts without Premium can opt in to transcribing voice and video messages
on the device using whisper.cpp and the multilingual Whisper small model.
Premium accounts always retain Telegram's standard server transcription,
including its existing result updates and rating controls. macOS builds require
macOS 10.15 or newer for the recognition library. The existing button
expands the text under the message. Telegram Premium and Telegram's trial
quota are not required. Text summaries remain a separate Telegram feature.

For a non-Premium account, clicking the transcription button offers to enable
local recognition, with a one-time download of the 465 MiB model from Hugging
Face if it is missing. Having a model on disk does not opt an account in.
"Don't show this offer again" persists independently of enabling recognition;
with local recognition off and the offer dismissed, the button uses Telegram's
normal trial, free-group, and Premium-limit behavior. Settings > Advanced >
Local transcription always provides the enable switch, offer preference,
model status, download, retry, and cancellation. Closing settings does not cancel
a download. Preferences are stored per account on this device. A downloaded
model can be reused when switching accounts, but each account opts in separately.
 The model revision, size, and SHA-256 are pinned in the source. Downloads
use HTTPS, are size-limited, and are committed atomically only after checksum
verification. Existing model files are checked before inference. No audio or
transcripts are sent to the model host or any other recognition service.

The model is shared by accounts in the application's `tdata/models` directory.
Audio uses Telegram's existing download/cache path. Recordings too large for
its memory cache use a private temporary directory, removed after processing
or cancellation; decoded samples and transcripts are kept in memory. Transcripts are not synced to other clients.
Self-destructing media is excluded. Removing a message or ending a session
cancels its outstanding recognition. Model downloads can continue independently
of a message and can be cancelled in settings. Clicking the button again while processing
also cancels that request. Failures can be retried by clicking the
button again. Work is serialized per account and runs off the UI thread.

The initial language is detected automatically, including Russian. Quality and
speed depend on the recording and hardware. Technical input limits are 64 MiB
of compressed audio and one hour of decoded audio. The recognizer rejects
external resources referenced by media containers. It does not execute a
shell or load third-party recognition plugins.

## Decisions

- Use whisper.cpp, pinned as a submodule, for the same offline engine across
  desktop platforms. Its code and the model weights use the MIT license.
- Use Whisper small for a balance of multilingual quality, download size,
  memory, and speed. A paid cloud API would not meet the requested cost and
  privacy requirements; OS speech services have platform/language availability
  restrictions.
- Retain the existing transcript UI and isolate recognition/model handling
  from message rendering. Telegram's transcription API is not used by this
  local path; summary and other Premium features keep their existing behavior.
- Keep local recognition opt-in for each non-Premium account; preserve the
  upstream server path for Premium and for users who decline local recognition.
  An explicit enable/download action authorizes fetching a missing model.
  Never fall back to online recognition after an error in the local path.
- Share the downloaded model across accounts, but keep enable and offer-dismissal
  preferences separate per account in the generic KV settings. This avoids
  changing the sequential session settings format.
- Allow downloads without choosing a message. Cancelling a transcription leaves
  the model download running; settings provide an explicit download cancellation.
  Interrupted downloads can be retried from the beginning.

## Verification

The focused `test_local_transcription` target checks malformed input,
cancellation, sample conversion, duration bounds, and attempts to reference
external media. Its optional model/audio arguments exercise the actual engine.
A successful build does not establish authenticated chat behavior; check a
voice message and a video message in the installed application separately.
