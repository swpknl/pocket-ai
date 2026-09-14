# Pocket AI

A voice-driven AI assistant for the M5Stack StickS3. Hold a button, ask a
question out loud, and get a spoken question transcribed and answered by an
LLM, shown on the built-in screen.

## How it works

1. Hold **Button A** and speak; release to stop recording (max 30 seconds).
2. The recording is uploaded to [OpenRouter](https://openrouter.ai) for
   transcription (`openai/whisper-large-v3`).
3. The transcribed text is sent to a chat model (`openrouter/free`) and the
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
3. Update `upload_port` / `monitor_port` in `platformio.ini` to match your
   device's serial port.
4. Build and upload:
   ```
   pio run --target upload
   ```
5. Open the serial monitor:
   ```
   pio device monitor
   ```

## Notes

- TLS certificate verification is disabled (`setInsecure()`) for
  prototyping. Replace with `setCACert()` and proper clock sync before
  relying on this for anything sensitive.
- Audio is buffered in PSRAM as 16-bit mono PCM at 16 kHz.
