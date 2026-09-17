# Local voice transcription

Voice messages and video messages are transcribed on the device using
whisper.cpp and the multilingual Whisper small model. macOS builds require
macOS 10.15 or newer for the recognition library. The existing button
expands the text under the message. Telegram Premium and Telegram's trial
quota are not required. Text summaries remain a separate Telegram feature.

The first use offers a one-time download of the 465 MiB model from Hugging
Face. The model revision, size, and SHA-256 are pinned in the source. Downloads
use HTTPS, are size-limited, and are committed atomically only after checksum
verification. Existing model files are checked before inference. No audio or
transcripts are sent to the model host or any other recognition service.

The model is shared by accounts in the application's `tdata/models` directory.
Audio uses Telegram's existing download/cache path. Recordings too large for
its memory cache use a private temporary directory, removed after processing
or cancellation; decoded samples and transcripts are kept in memory. Transcripts are not synced to other clients.
Self-destructing media is excluded. Removing a message or ending a session
cancels its outstanding recognition. Clicking the button again while processing
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
- Download only after an explicit first-use action; do not bundle the model in
  every application update. Never fall back silently to online recognition.

## Verification

The focused `test_local_transcription` target checks malformed input,
cancellation, sample conversion, duration bounds, and attempts to reference
external media. Its optional model/audio arguments exercise the actual engine.
A successful build does not establish authenticated chat behavior; check a
voice message and a video message in the installed application separately.
