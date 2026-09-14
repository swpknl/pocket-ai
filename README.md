# Pocket AI

A voice-driven AI assistant for the M5Stack StickS3. Hold a button, ask a
question out loud, and get a spoken question transcribed and answered by an
LLM, shown on the built-in screen.

## How it works

1. Hold **Button A** and speak; release to stop recording (max 30 seconds).
2. The recording is uploaded to [OpenRouter](https://openrouter.ai) for
   transcription by a speech-to-text model (`openai/whisper-large-v3-turbo`).
3. The transcribed text is sent to a chat model (`inclusionai/ling-3.0-flash-vl:free`) and the
   reply is shown on screen.
4. You can also type a question into the Serial Monitor instead of speaking;
   the reply is shown the same way.

### Screens

- **Home** - instructions and Wi-Fi status. Shown at boot and whenever you
  hold **Button B** for one second.
- **Answer** - shown after a question is answered. Short-press **Button B**
  to page through long replies.
- **Battery** - short-press **Button B** from the home screen to view
  battery level and charging status; it auto-refreshes every 2 seconds
  while shown.

Holding **Button B** for one second always returns to the home screen.

## Hardware

- M5Stack StickS3 (ESP32-S3, built-in mic and display)

## Setup

1. Install [PlatformIO](https://platformio.org/).
2. Copy `include/secrets.h.example` to `include/secrets.h` and fill in your
   Wi-Fi credentials and [OpenRouter API key](https://openrouter.ai/keys).
   This file is gitignored and should never be committed.
3. Build and upload (PlatformIO auto-detects the device's serial port on
   both Windows and macOS):
   ```
   pio run --target upload
   ```
4. Open the serial monitor:
   ```
   pio device monitor
   ```

## Notes

- TLS certificate verification is disabled (`setInsecure()`) for
  prototyping. Replace with `setCACert()` and proper clock sync before
  relying on this for anything sensitive.
- Audio is buffered in PSRAM as 16-bit mono PCM at 16 kHz.


## Sleep mode

- After two minutes without button or serial input, the stick enters deep sleep.
- Hold **B for three seconds**, then release, to sleep manually. The existing
  one-second hold still returns home.
- Press **A** to wake. This resets the chip: the stick reruns startup, so wake
  takes a few seconds and always lands on the home screen (no answer or page
  is retained across sleep).
- The display, Wi-Fi, microphone and speaker are stopped before sleeping, and
  power draw during sleep is far lower than a suspend/resume ("light") sleep
  would give, at the cost of losing in-memory state on wake.
- Recording and network requests finish before the idle timer starts again.
  An unfinished Serial Monitor question prevents automatic sleep.
- USB serial is unavailable during sleep; use A to wake and reconnect the
  monitor if needed.
- Change `IDLE_SLEEP_MS` in `src/main.cpp` to adjust the idle timeout.

Hardware validation after uploading: check manual sleep, idle sleep, and A
wake (confirm it boots to home without an accidental recording starting).
Repeat on battery and USB power.


## Chat model

Pinned to `inclusionai/ling-3.0-flash-vl:free` for chat responses, with a
fallback to `meta-llama/llama-3.3-70b-instruct:free` if the primary is
rate-limited, avoiding random selection of specialized models such as
safety classifiers. Free availability and rate limits can change. The model
returned by OpenRouter is logged in Serial Monitor for every successful
chat response, including empty answers.

A newer release does not guarantee a newer knowledge cutoff. The stick has
no web-search integration, so current events and other live facts may still
be outdated. Changing the model does not add internet access.

## Speech-to-text model

Pinned to `openai/whisper-large-v3-turbo` via OpenRouter for transcribing
recorded audio before it is sent to the chat model. Accuracy depends on
this model and on microphone input quality; there is no local/offline
fallback if OpenRouter is unreachable.
