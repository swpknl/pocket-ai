#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

#include "secrets.h"

// secrets.h defines:
// WIFI_SSID
// WIFI_PASSWORD
// OPENROUTER_API_KEY

const char *CHAT_URL =
    "https://openrouter.ai/api/v1/chat/completions";

const char *TRANSCRIBE_URL =
    "https://openrouter.ai/api/v1/audio/transcriptions";

// Pin chat models so the free router cannot select a safety classifier.
// CHAT_MODEL_FALLBACK is retried once if the primary is rate-limited or
// unavailable (OpenRouter's free tier deprecates/limits models often).
const char *CHAT_MODEL = "inclusionai/ling-3.0-flash-vl:free";
const char *CHAT_MODEL_FALLBACK = "meta-llama/llama-3.3-70b-instruct:free";
const char *TRANSCRIBE_MODEL = "openai/whisper-large-v3";

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr unsigned long IDLE_SLEEP_MS = 2UL * 60UL * 1000UL;
constexpr unsigned long SLEEP_HOLD_MS = 3000;
// StickS3 Button A is active-low on GPIO11.
constexpr gpio_num_t WAKE_BUTTON_PIN = GPIO_NUM_11;
constexpr size_t CHUNK_SAMPLES = 1600;
constexpr size_t MAX_RECORD_SECONDS = 30;
constexpr size_t MAX_SAMPLES =
    SAMPLE_RATE * MAX_RECORD_SECONDS;

enum class Screen
{
  Home,
  Text,
  Battery,
  Status
};

Screen currentScreen = Screen::Home;

String screenText;
String serialQuestion;

size_t pageStart = 0;
size_t nextPageStart = 0;

bool waitForARelease = false;
bool waitForBRelease = false;
bool bHoldHandled = false;
bool sleepPending = false;
unsigned long lastActivity = 0;

unsigned long lastBatteryRefresh = 0;

// ---------- Display ----------

void showStatus(const String &text)
{
  currentScreen = Screen::Status;

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(4, 4);
  M5.Display.println(text);

  Serial.println(text);
}

void showPage()
{
  currentScreen = Screen::Text;

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 0);

  // Default font at size 2: 12 pixels wide, 16 high.
  const int charWidth = 12;
  const int lineHeight = 16;
  const int footerHeight = 20;

  const int columns =
      M5.Display.width() / charWidth;

  const int rows =
      (M5.Display.height() - footerHeight) / lineHeight;

  size_t position = pageStart;

  for (int row = 0;
       row < rows && position < screenText.length();
       ++row)
  {
    String line;

    while (
        position < screenText.length() &&
        line.length() < static_cast<unsigned>(columns))
    {
      const char ch = screenText[position++];

      if (ch == '\r')
        continue;
      if (ch == '\n')
        break;

      line += ch;
    }

    M5.Display.setCursor(0, row * lineHeight);
    M5.Display.print(line);
  }

  nextPageStart = position;

  M5.Display.setCursor(
      0,
      M5.Display.height() - lineHeight);

  M5.Display.print(
      nextPageStart < screenText.length()
          ? "B:next  Hold B:home"
          : "B:first Hold B:home");
}

void showMessage(const String &text)
{
  screenText = text;
  pageStart = 0;

  showPage();
  Serial.println(text);
}

void showHome()
{
  currentScreen = Screen::Home;

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 0);

  M5.Display.println("Pocket AI");
  M5.Display.println("Hold A: speak");
  M5.Display.println("Release A: send");
  M5.Display.printf(
      "Max: %u seconds\n",
      static_cast<unsigned>(MAX_RECORD_SECONDS));
  M5.Display.println("B: battery");
  M5.Display.println("B 1s:home 3s:sleep");
  M5.Display.println(
      WiFi.status() == WL_CONNECTED
          ? "WiFi: connected"
          : "WiFi: offline");

  Serial.println(
      "\nPocket AI ready. Hold A to speak, "
      "or type a question in Serial Monitor.");
}

void showBatteryPage()
{
  currentScreen = Screen::Battery;
  lastBatteryRefresh = millis();

  const int level = M5.Power.getBatteryLevel();
  const bool charging = M5.Power.isCharging();

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 0);

  M5.Display.println("Battery");
  M5.Display.println();

  if (level >= 0)
  {
    M5.Display.printf("Level: %d%%\n", level);
  }
  else
  {
    M5.Display.println("Level: unknown");
  }

  M5.Display.println(
      charging ? "Charging" : "Not charging");

  M5.Display.println();
  M5.Display.println("Hold A: ask");
  M5.Display.println("B 1s:home 3s:sleep");
}

// Ignore buttons held during blocking network operations.
void resetButtonState()
{
  M5.update();

  waitForARelease = true;
  waitForBRelease = true;
  bHoldHandled = false;
  sleepPending = false;
  lastActivity = millis();
}

bool connectWiFi(bool showFailure = true);

// Deep sleep draws far less current than light sleep. Waking resets the
// chip and reruns setup() from scratch, so no RAM state (screen, answer,
// page position) survives -- the device always boots back to the home
// screen after sleep.
void enterSleep()
{
  if (M5.getBoard() != m5::board_t::board_M5StickS3)
  {
    showMessage("Sleep requires StickS3.");
    resetButtonState();
    return;
  }

  const esp_err_t result =
      esp_sleep_enable_ext0_wakeup(WAKE_BUTTON_PIN, 0);

  if (result != ESP_OK)
  {
    showMessage("Could not configure wake button.");
    resetButtonState();
    return;
  }

  M5.Mic.end();
  M5.Speaker.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("Deep sleeping. Press A to wake.");
  Serial.flush();
  M5.Display.sleep();
  M5.Display.waitDisplay();

  esp_deep_sleep_start();
  // Never reached: deep sleep resets the chip.
}

// ---------- Wi-Fi ----------

bool connectWiFi(bool showFailure)
{
  if (WiFi.status() == WL_CONNECTED)
    return true;

  showStatus("Connecting...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long started = millis();

  while (
      WiFi.status() != WL_CONNECTED &&
      millis() - started < 30000)
  {
    M5.update();
    delay(20);
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    // Wake reconnects preserve the existing answer even if Wi-Fi is unavailable.
    if (showFailure)
      showMessage(
        "WiFi failed.\n"
        "Check your iPhone hotspot and\n"
        "Maximize Compatibility setting.\n\n"
        "Hold A to retry.");
    return false;
  }

  return true;
}

bool readyForRequest()
{
  if (
      strlen(OPENROUTER_API_KEY) == 0 ||
      String(OPENROUTER_API_KEY) == "YOUR_OPENROUTER_API_KEY")
  {
    showMessage(
        "Set your OpenRouter API key in secrets.h.");
    return false;
  }

  return connectWiFi();
}

// ---------- HTTP ----------

bool isRetryableStatus(int status)
{
  return status == 429 || status == 404 || status == 503;
}

bool postRequest(
    const char *url,
    const String &contentType,
    uint8_t *body,
    size_t bodyLength,
    JsonDocument &response,
    int *statusOut = nullptr)
{
  WiFiClientSecure tls;

  // Prototype only: skips server certificate verification.
  // For regular use, configure a trusted CA certificate
  // and synchronize the device clock.
  tls.setInsecure();

  HTTPClient http;

  // Preserve the settings that worked on your device.
  http.useHTTP10(true);
  http.setReuse(false);
  http.setConnectTimeout(15000);
  http.setTimeout(60000);

  if (!http.begin(tls, url))
  {
    showMessage("Could not initialize HTTPS.");
    return false;
  }

  http.addHeader("Content-Type", contentType);
  http.addHeader("Accept", "application/json");
  http.addHeader(
      "Authorization",
      String("Bearer ") + OPENROUTER_API_KEY);

  const int status = http.POST(body, bodyLength);

  if (statusOut)
    *statusOut = status;

  if (status <= 0)
  {
    const String error =
        HTTPClient::errorToString(status);

    http.end();
    showMessage("Connection error:\n" + error);
    return false;
  }

  const int expectedSize = http.getSize();
  String responseBody = http.getString();

  http.end();

  Serial.printf(
      "\nHTTP: %d | Expected: %d | Received: %u\n",
      status,
      expectedSize,
      static_cast<unsigned>(responseBody.length()));

  if (responseBody.isEmpty())
  {
    showMessage(
        String("HTTP ") + status +
        "\nEmpty response.\nPlease try again.");
    return false;
  }

  const DeserializationError error =
      deserializeJson(response, responseBody);

  if (error)
  {
    Serial.print("JSON error: ");
    Serial.println(error.c_str());
    Serial.println(responseBody.substring(0, 1000));

    showMessage(
        String("HTTP ") + status +
        "\nJSON: " + error.c_str() +
        "\nSee Serial Monitor.");
    return false;
  }

  if (status != 200 || !response["error"].isNull())
  {
    const char *message =
        response["error"]["message"] | "Request failed";

    Serial.print("API error: ");
    serializeJson(response["error"], Serial);
    Serial.println();

    showMessage(
        String("HTTP ") + status + "\n" + message);
    return false;
  }

  return true;
}

// ---------- Chat ----------

void askQuestion(const String &question)
{
  if (!readyForRequest())
    return;

  Serial.println("\nQuestion: " + question);
  showStatus("Thinking...");

  const char *models[] = {CHAT_MODEL, CHAT_MODEL_FALLBACK};
  constexpr size_t modelCount = sizeof(models) / sizeof(models[0]);

  JsonDocument response;
  bool ok = false;
  int status = 0;

  for (size_t i = 0; i < modelCount; ++i)
  {
    if (i > 0)
    {
      Serial.println("Retrying with fallback model...");
      showStatus("Retrying...");
    }

    String body;

    {
      JsonDocument request;

      request["model"] = models[i];
      request["stream"] = false;
      request["max_tokens"] = 1024;

      JsonArray messages =
          request["messages"].to<JsonArray>();

      JsonObject system = messages.add<JsonObject>();
      system["role"] = "system";
      system["content"] =
          "You are a helpful assistant on a tiny screen. "
          "Keep your final answer under 100 words. "
          "Use plain English and ASCII characters. "
          "Do not use markdown or emoji.";

      JsonObject user = messages.add<JsonObject>();
      user["role"] = "user";
      user["content"] = question;

      serializeJson(request, body);
    }

    response.clear();

    ok = postRequest(
        CHAT_URL,
        "application/json",
        reinterpret_cast<uint8_t *>(
            const_cast<char *>(body.c_str())),
        body.length(),
        response,
        &status);

    if (ok || !isRetryableStatus(status))
      break;
  }

  if (!ok)
    return;

  Serial.print("Model: ");
  Serial.println(response["model"] | "unknown");

  String reply;

  if (
      response["choices"][0]["message"]["content"]
          .is<const char *>())
  {
    reply =
        response["choices"][0]["message"]["content"]
            .as<String>();
  }

  reply.trim();

  if (reply.isEmpty())
  {
    const char *finish =
        response["choices"][0]["finish_reason"] | "unknown";

    Serial.print("Finish: ");
    Serial.println(finish);

    Serial.print("Usage: ");
    serializeJson(response["usage"], Serial);
    Serial.println();

    showMessage(
        String("No answer text.\nFinish: ") + finish +
        "\nSee Serial Monitor.");
    return;
  }

  showMessage(
      "You asked:\n" + question +
      "\n\nAnswer:\n" + reply);
}

// ---------- WAV formatting ----------

void put16(uint8_t *p, uint16_t value)
{
  p[0] = value & 0xff;
  p[1] = (value >> 8) & 0xff;
}

void put32(uint8_t *p, uint32_t value)
{
  p[0] = value & 0xff;
  p[1] = (value >> 8) & 0xff;
  p[2] = (value >> 16) & 0xff;
  p[3] = (value >> 24) & 0xff;
}

void writeWavHeader(uint8_t *wav, size_t samples)
{
  const uint32_t pcmBytes =
      samples * sizeof(int16_t);

  memcpy(wav, "RIFF", 4);
  put32(wav + 4, 36 + pcmBytes);

  memcpy(wav + 8, "WAVEfmt ", 8);
  put32(wav + 16, 16);
  put16(wav + 20, 1); // PCM
  put16(wav + 22, 1); // Mono
  put32(wav + 24, SAMPLE_RATE);
  put32(wav + 28, SAMPLE_RATE * 2);
  put16(wav + 32, 2);  // Block alignment
  put16(wav + 34, 16); // Bits per sample

  memcpy(wav + 36, "data", 4);
  put32(wav + 40, pcmBytes);
}

// ---------- Transcription ----------

String transcribe(uint8_t *wav, size_t wavLength)
{
  showStatus("Transcribing...");

  const String boundary = "----PocketAI7cf942e1";

  String head = "--" + boundary + "\r\n";
  head +=
      "Content-Disposition: form-data; "
      "name=\"model\"\r\n\r\n";
  head += TRANSCRIBE_MODEL;

  head += "\r\n--" + boundary + "\r\n";
  head +=
      "Content-Disposition: form-data; "
      "name=\"file\"; filename=\"question.wav\"\r\n";
  head += "Content-Type: audio/wav\r\n\r\n";

  const String tail =
      "\r\n--" + boundary + "--\r\n";

  const size_t total =
      head.length() + wavLength + tail.length();

  uint8_t *upload = static_cast<uint8_t *>(
      heap_caps_malloc(
          total,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (!upload)
  {
    showMessage("Not enough PSRAM for audio upload.");
    return "";
  }

  memcpy(upload, head.c_str(), head.length());

  memcpy(
      upload + head.length(),
      wav,
      wavLength);

  memcpy(
      upload + head.length() + wavLength,
      tail.c_str(),
      tail.length());

  JsonDocument response;

  const bool ok = postRequest(
      TRANSCRIBE_URL,
      "multipart/form-data; boundary=" + boundary,
      upload,
      total,
      response);

  heap_caps_free(upload);

  if (!ok)
    return "";

  String text;

  if (response["text"].is<const char *>())
  {
    text = response["text"].as<String>();
  }

  text.trim();

  if (text.isEmpty())
  {
    showMessage(
        "No speech recognized.\n"
        "Hold A and speak clearly,\n"
        "then release.");
    return "";
  }

  Serial.println("\nTranscribed question: " + text);
  return text;
}

// ---------- Recording ----------

void recordQuestion()
{
  if (!readyForRequest())
    return;

  M5.update();

  // Connecting may have taken time.
  if (!M5.BtnA.isPressed())
  {
    showMessage("Connected.\nHold A again to record.");
    return;
  }

  const size_t capacity =
      44 + MAX_SAMPLES * sizeof(int16_t);

  uint8_t *wav = static_cast<uint8_t *>(
      heap_caps_malloc(
          capacity,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (!wav)
  {
    showMessage(
        "Audio needs PSRAM.\n"
        "Check your PlatformIO PSRAM settings.");
    return;
  }

  M5.Speaker.end();

  if (!M5.Mic.begin())
  {
    M5.Mic.end();
    heap_caps_free(wav);

    showMessage(
        "Microphone failed to start.\n"
        "Check M5Unified version.");
    return;
  }

  int16_t *pcm =
      reinterpret_cast<int16_t *>(wav + 44);

  size_t samples = 0;
  bool failed = false;

  showStatus(
      String("Recording...\nRelease A\nto send.\nMax: ") +
      static_cast<unsigned>(MAX_RECORD_SECONDS) + "s");

  while (samples + CHUNK_SAMPLES <= MAX_SAMPLES)
  {
    M5.update();

    if (!M5.BtnA.isPressed())
      break;

    if (!M5.Mic.record(
            pcm + samples,
            CHUNK_SAMPLES,
            SAMPLE_RATE,
            false))
    {
      failed = true;
      break;
    }

    const unsigned long started = millis();

    while (M5.Mic.isRecording())
    {
      M5.update();
      delay(1);

      if (millis() - started > 2000)
      {
        failed = true;
        break;
      }
    }

    if (failed)
      break;

    samples += CHUNK_SAMPLES;
  }

  M5.Mic.end();

  if (failed)
  {
    heap_caps_free(wav);
    showMessage(
        "Microphone recording failed.\nPlease retry.");
    return;
  }

  if (samples < SAMPLE_RATE / 2)
  {
    heap_caps_free(wav);

    showMessage(
        "Recording too short.\n"
        "Hold A while speaking,\n"
        "then release.");
    return;
  }

  Serial.printf(
      "Recorded %.1f seconds\n",
      static_cast<double>(samples) / SAMPLE_RATE);

  writeWavHeader(wav, samples);

  const String question = transcribe(
      wav,
      44 + samples * sizeof(int16_t));

  heap_caps_free(wav);

  if (!question.isEmpty())
  {
    askQuestion(question);
  }
}

// ---------- Setup ----------

void setup()
{
  auto cfg = M5.config();
  cfg.internal_mic = true;

  M5.begin(cfg);
  Serial.begin(115200);

  M5.Display.setRotation(1);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  // Voice input only.
  M5.Speaker.end();
  M5.Mic.end();

  showStatus("Pocket AI");
  delay(500);

  if (connectWiFi())
  {
    showHome();
  }

  resetButtonState();
}

// ---------- Main loop ----------

void loop()
{
  M5.update();

  if (M5.BtnA.isPressed() || M5.BtnB.isPressed())
    lastActivity = millis();

  // A: hold to record.
  if (waitForARelease)
  {
    if (!M5.BtnA.isPressed())
    {
      waitForARelease = false;
    }
  }
  else if (M5.BtnA.wasPressed())
  {
    recordQuestion();
    resetButtonState();
    return;
  }

  // B: short press depends on the current screen.
  // Hold for one second returns home.
  if (waitForBRelease)
  {
    if (!M5.BtnB.isPressed())
    {
      waitForBRelease = false;
    }
  }
  else
  {
    if (
        M5.BtnB.isPressed() &&
        M5.BtnB.pressedFor(1000) &&
        !bHoldHandled)
    {
      bHoldHandled = true;
      showHome();
    }

    if (M5.BtnB.isPressed() && M5.BtnB.pressedFor(SLEEP_HOLD_MS))
      sleepPending = true;

    if (M5.BtnB.wasReleased())
    {
      if (sleepPending && !M5.BtnA.isPressed())
      {
        enterSleep();
        return;
      }
      sleepPending = false;
      if (!bHoldHandled)
      {
        if (currentScreen == Screen::Home)
        {
          showBatteryPage();
        }
        else if (
            currentScreen == Screen::Text &&
            !screenText.isEmpty())
        {
          pageStart =
              nextPageStart < screenText.length()
                  ? nextPageStart
                  : 0;

          showPage();
        }
        else if (
            currentScreen == Screen::Battery)
        {
          showBatteryPage();
        }
      }

      bHoldHandled = false;
    }
  }

  // Refresh battery information only on its own page.
  if (
      currentScreen == Screen::Battery &&
      millis() - lastBatteryRefresh >= 2000)
  {
    showBatteryPage();
  }

  // Typed questions remain available over USB.
  while (Serial.available())
  {
    lastActivity = millis();
    const char ch = Serial.read();

    if (ch == '\r')
      continue;

    if (ch == '\n')
    {
      serialQuestion.trim();

      if (!serialQuestion.isEmpty())
      {
        askQuestion(serialQuestion);
        serialQuestion = "";
        resetButtonState();
        return;
      }

      serialQuestion = "";
    }
    else if (serialQuestion.length() < 2000)
    {
      serialQuestion += ch;
    }
  }

  if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed() &&
      serialQuestion.isEmpty() && millis() - lastActivity >= IDLE_SLEEP_MS)
  {
    enterSleep();
    return;
  }

  delay(10);
}