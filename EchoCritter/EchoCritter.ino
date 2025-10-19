#include <M5Cardputer.h>
#include <esp_heap_caps.h>
#include <cstdarg>
#include <cmath>

// -----------------------------------------------------------------------------
// Echo Critter v1
// Press and hold the GO/ENTER button to capture up to three seconds of audio.
// Playback always returns the recording at a playful chipmunk (~1.4x) speed.
// -----------------------------------------------------------------------------

constexpr uint32_t SAMPLE_RATE = 16000;           // Microphone & speaker base rate
constexpr uint32_t MAX_RECORD_MS = 3000;          // Maximum record length (3 seconds)
constexpr size_t MAX_SAMPLES = SAMPLE_RATE * MAX_RECORD_MS / 1000;
constexpr size_t BUFFER_BYTES = MAX_SAMPLES * sizeof(int16_t);
constexpr uint32_t FACE_BLINK_INTERVAL = 120;     // milliseconds between playface blinks
constexpr size_t RECORD_CHUNK_SAMPLES = 256;      // Samples per I2S read block
constexpr float CHIPMUNK_MULTIPLIER = 1.4f;       // Playback rate multiplier
constexpr bool ENABLE_DEBUG_LOG = true;           // Toggle verbose serial logging

enum class FaceState { Idle, Recording, Playing };

// -----------------------------------------------------------------------------
// Logging helper
// -----------------------------------------------------------------------------

namespace Log {
  void print(const char *fmt, ...) {
    if (!ENABLE_DEBUG_LOG) {
      return;
    }
    va_list args;
    va_start(args, fmt);
    Serial.printf("[Echo] ");
    Serial.vprintf(fmt, args);
    Serial.println();
    va_end(args);
  }
}

// -----------------------------------------------------------------------------
// Display controller
// -----------------------------------------------------------------------------

class DisplayController {
public:
  void begin() {
    M5.Display.setRotation(1);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.fillScreen(TFT_BLACK);
  }

  void showSplash() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextSize(3);
    M5.Display.drawString("Echo Critter v1", M5.Display.width() / 2, M5.Display.height() / 2 - 20);
    M5.Display.setTextSize(2);
    M5.Display.drawString("Hold GO to record", M5.Display.width() / 2, M5.Display.height() / 2 + 20);
    delay(2000);
    M5.Display.fillScreen(TFT_BLACK);
    updateStatus(false, false);
    updateFace(FaceState::Idle, 0.0f);
  }

  void updateFace(FaceState state, float meter, bool blinkPhase = false) {
    if (state == lastFace && std::fabs(lastMeter - meter) < 0.02f && blinkPhase == lastBlinkPhase) {
      return;
    }

    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.fillRect(0, 40, M5.Display.width(), 140, TFT_BLACK);

    const char *faceText = "(o_o)";
    switch (state) {
      case FaceState::Idle:
        faceText = "(o_o)";
        M5.Display.setTextSize(2);
        M5.Display.drawString("Hold GO to record", M5.Display.width() / 2, 150);
        M5.Display.fillRect(0, 180, M5.Display.width(), 50, TFT_BLACK);
        break;
      case FaceState::Recording:
        faceText = "(^-^)";
        drawRecordingHud(meter);
        break;
      case FaceState::Playing:
        faceText = blinkPhase ? "(*>w<)" : "(*>w<)♪";
        break;
    }

    M5.Display.setTextSize(4);
    M5.Display.drawString(faceText, M5.Display.width() / 2, 100);

    lastFace = state;
    lastMeter = meter;
    lastBlinkPhase = blinkPhase;
  }

  void updateStatus(bool recording, bool hasRecording) {
    int state = recording ? 1 : (hasRecording ? 2 : 0);
    if (state == lastStatus) {
      return;
    }

    const char *label = nullptr;
    switch (state) {
      case 1: label = "Recording..."; break;
      case 2: label = "Release to play"; break;
      default: label = "Hold GO to record"; break;
    }

    M5.Display.fillRoundRect(10, 10, 220, 36, 8, TFT_DARKGREY);
    M5.Display.drawRoundRect(10, 10, 220, 36, 8, TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::middle_left);
    M5.Display.setTextSize(2);
    M5.Display.drawString(label, 24, 28);

    lastStatus = state;
  }

private:
  void drawRecordingHud(float level) {
    uint16_t barWidth = static_cast<uint16_t>((M5.Display.width() - 40) * constrain(level, 0.0f, 1.0f));
    uint16_t barHeight = 16;
    int16_t x = 20;
    int16_t y = 190;

    M5.Display.fillRect(x, y, M5.Display.width() - 40, barHeight, TFT_DARKGREY);
    M5.Display.fillRect(x, y, barWidth, barHeight, TFT_GREEN);

    M5.Display.setTextDatum(textdatum_t::top_center);
    M5.Display.setTextSize(1);
    M5.Display.drawString("LISTENING", M5.Display.width() / 2, y - 14);
  }

  FaceState lastFace = FaceState::Idle;
  float lastMeter = -1.0f;
  bool lastBlinkPhase = false;
  int lastStatus = -1;
};

// -----------------------------------------------------------------------------
// Input controller
// -----------------------------------------------------------------------------

class InputController {
public:
  void begin() {
    keyboardEnabled = true;
  }

  void update() {
    M5Cardputer.update();

    bool kbEnter = keyboardEnabled ? M5Cardputer.Keyboard.keysState().enter : false;
    bool button = M5.BtnA.isPressed();
    current = kbEnter || button;
    justPressedFlag = current && !previous;
    releasedFlag = !current && previous;
    previous = current;
  }

  bool isPressed() const { return current; }
  bool justPressed() const { return justPressedFlag; }
  bool released() const { return releasedFlag; }

private:
  bool keyboardEnabled = false;
  bool current = false;
  bool previous = false;
  bool justPressedFlag = false;
  bool releasedFlag = false;
};

// -----------------------------------------------------------------------------
// Audio engine
// -----------------------------------------------------------------------------

class AudioEngine {
public:
  bool begin() {
    buffer = static_cast<int16_t *>(heap_caps_malloc(BUFFER_BYTES, MALLOC_CAP_8BIT | MALLOC_CAP_DMA));
    if (!buffer) {
      buffer = static_cast<int16_t *>(ps_malloc(BUFFER_BYTES));
    }
    if (!buffer) {
      Log::print("FATAL: audio buffer allocation failed");
      return false;
    }

    auto micCfg = M5.Mic.config();
    micCfg.sample_rate = SAMPLE_RATE;
    micCfg.stereo = false;
    micCfg.dma_buf_count = 6;
    micCfg.dma_buf_len = 256;
    M5.Mic.config(micCfg);
    Log::print("Mic config set: rate=%u stereo=%d dma=%u/%u",
               micCfg.sample_rate,
               micCfg.stereo,
               static_cast<unsigned>(micCfg.dma_buf_count),
               static_cast<unsigned>(micCfg.dma_buf_len));

    auto spkCfg = M5.Speaker.config();
    spkCfg.sample_rate = SAMPLE_RATE;
    spkCfg.stereo = false;
    spkCfg.dma_buf_len = 256;
    spkCfg.dma_buf_count = 6;
    M5.Speaker.config(spkCfg);
    M5.Speaker.setVolume(255);
    M5.Speaker.setAllChannelVolume(255);
    Log::print("Speaker config set: rate=%u stereo=%d dma=%u/%u volume=%u",
               spkCfg.sample_rate,
               spkCfg.stereo,
               static_cast<unsigned>(spkCfg.dma_buf_count),
               static_cast<unsigned>(spkCfg.dma_buf_len),
               static_cast<unsigned>(M5.Speaker.getVolume()));

    M5.Speaker.end();
    bool micOk = M5.Mic.begin();
    Log::print("Mic.begin -> %s", micOk ? "ok" : "FAILED");
    return micOk;
  }

  void startRecording() {
    if (M5.Speaker.isRunning()) {
      M5.Speaker.stop();
    }
    M5.Speaker.end();
    if (!M5.Mic.isRunning()) {
      M5.Mic.begin();
    }

    recordedSamples = 0;
    hasRecordingFlag = false;
    recording = true;
    recordStart = millis();
    level = 0.0f;
    levelTimestamp = recordStart;

    Log::print("Recording started");
  }

  void stopRecording() {
    if (!recording) {
      return;
    }
    recording = false;
    while (M5.Mic.isRecording() != 0) {
      delay(2);
    }
    hasRecordingFlag = recordedSamples > 0;
    Log::print("Recording complete, samples=%u", static_cast<unsigned>(recordedSamples));
  }

  void updateRecording() {
    if (!recording || !buffer) {
      return;
    }

    if (recordedSamples >= MAX_SAMPLES) {
      stopRecording();
      return;
    }

    size_t remaining = MAX_SAMPLES - recordedSamples;
    size_t chunkSamples = (remaining < RECORD_CHUNK_SAMPLES) ? remaining : RECORD_CHUNK_SAMPLES;
    int16_t *dst = buffer + recordedSamples;

    if (M5.Mic.record(dst, chunkSamples, SAMPLE_RATE, false)) {
      double accum = 0.0;
      for (size_t i = 0; i < chunkSamples; ++i) {
        float s = dst[i] / 32768.0f;
        accum += s * s;
      }
      level = (chunkSamples > 0)
          ? constrain(sqrtf(accum / static_cast<double>(chunkSamples)) * 4.0f, 0.0f, 1.0f)
          : 0.0f;
      recordedSamples += chunkSamples;
      levelTimestamp = millis();
      hasRecordingFlag = recordedSamples > 0;
      Log::print("Captured chunk=%u total=%u meter=%.2f",
                 static_cast<unsigned>(chunkSamples),
                 static_cast<unsigned>(recordedSamples),
                 level);
    } else if (millis() - levelTimestamp > 60) {
      level *= 0.92f;
      if (level < 0.01f) {
        level = 0.0f;
      }
      levelTimestamp = millis();
    }
  }

  bool playChipmunk(DisplayController &display) {
    if (!hasRecordingFlag || recordedSamples == 0) {
      Log::print("playChipmunk aborted: no recording");
      return false;
    }

    if (M5.Mic.isRunning()) {
      Log::print("Stopping mic before playback");
      while (M5.Mic.isRecording() != 0) {
        delay(2);
      }
      M5.Mic.end();
    }

    if (M5.Speaker.isRunning()) {
      M5.Speaker.stop();
      M5.Speaker.end();
    }

    bool spkOk = M5.Speaker.begin();
    Log::print("Speaker begin -> %s", spkOk ? "ok" : "FAILED");
    if (!spkOk) {
      return false;
    }
    M5.Speaker.setVolume(255);
    M5.Speaker.setAllChannelVolume(255);
    Log::print("Speaker volume=%u", static_cast<unsigned>(M5.Speaker.getVolume()));

    uint32_t playbackRate = static_cast<uint32_t>(SAMPLE_RATE * CHIPMUNK_MULTIPLIER);
    bool started = M5.Speaker.playRaw(buffer, recordedSamples, playbackRate, false, 1, 0);
    Log::print("Playback started samples=%u", static_cast<unsigned>(recordedSamples));

    uint32_t blinkTimer = millis();
    bool blinkPhase = false;
    display.updateFace(FaceState::Playing, 0.0f, blinkPhase);

    while (started && M5.Speaker.isPlaying()) {
      if (millis() - blinkTimer > FACE_BLINK_INTERVAL) {
        blinkTimer = millis();
        blinkPhase = !blinkPhase;
        display.updateFace(FaceState::Playing, 0.0f, blinkPhase);
      }
      delay(10);
      M5Cardputer.update();
    }

    M5.Speaker.stop();
    M5.Speaker.end();
    bool micOk = M5.Mic.begin();
    Log::print("Playback finished, mic resumed -> %s", micOk ? "ok" : "FAILED");

    return started;
  }

  bool isRecording() const { return recording; }
  bool hasRecording() const { return hasRecordingFlag; }
  float currentLevel() const { return level; }
  size_t sampleCount() const { return recordedSamples; }
  uint32_t recordingElapsed() const { return recording ? millis() - recordStart : 0; }

  void resetAfterPlayback() {
    recordedSamples = 0;
    hasRecordingFlag = false;
    level = 0.0f;
    levelTimestamp = millis();
  }

private:
  int16_t *buffer = nullptr;
  size_t recordedSamples = 0;
  bool recording = false;
  bool hasRecordingFlag = false;
  uint32_t recordStart = 0;
  float level = 0.0f;
  uint32_t levelTimestamp = 0;
};

// -----------------------------------------------------------------------------
// Application coordinator
// -----------------------------------------------------------------------------

class EchoApp {
public:
  void setup() {
    auto cfg = M5.config();
    cfg.output_power = true;
    cfg.internal_mic = true;
    cfg.internal_spk = true;
    cfg.serial_baudrate = ENABLE_DEBUG_LOG ? 115200 : cfg.serial_baudrate;
    if (ENABLE_DEBUG_LOG) {
      Serial.begin(115200);
      delay(50);
    }
    M5Cardputer.begin(cfg);
    input.begin();

    display.begin();
    display.showSplash();

    if (!audio.begin()) {
      while (true) { delay(1000); }
    }
  }

  void loop() {
    input.update();

    if (input.justPressed() && !audio.isRecording()) {
      audio.startRecording();
      display.updateStatus(true, false);
      display.updateFace(FaceState::Recording, 0.0f);
    }

    if (audio.isRecording()) {
      audio.updateRecording();
      display.updateFace(FaceState::Recording, audio.currentLevel());
      display.updateStatus(true, audio.hasRecording());
      if (audio.recordingElapsed() >= MAX_RECORD_MS) {
        audio.stopRecording();
      }
    }

    if (input.released()) {
      Log::print("Enter released, recordingActive=%d hasRecording=%d samples=%u",
                 audio.isRecording() ? 1 : 0,
                 audio.hasRecording() ? 1 : 0,
                 static_cast<unsigned>(audio.sampleCount()));

      if (audio.isRecording()) {
        audio.stopRecording();
      }

      if (audio.hasRecording()) {
        display.updateStatus(false, true);
        display.updateFace(FaceState::Playing, 0.0f);
        if (audio.playChipmunk(display)) {
          audio.resetAfterPlayback();
        }
      } else {
        display.updateFace(FaceState::Idle, 0.0f);
        display.updateStatus(false, false);
      }
    }

    if (!audio.isRecording() && !input.isPressed() && !audio.hasRecording()) {
      display.updateFace(FaceState::Idle, 0.0f);
      display.updateStatus(false, false);
    }
  }

private:
  DisplayController display;
  InputController input;
  AudioEngine audio;
};

// -----------------------------------------------------------------------------
// Arduino hooks
// -----------------------------------------------------------------------------

EchoApp app;

void setup() {
  app.setup();
}

void loop() {
  app.loop();
}
