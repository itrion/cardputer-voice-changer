#include <M5Cardputer.h>
#include <cmath>
#include <cstdarg>
#include <esp_heap_caps.h>

// -----------------------------------------------------------------------------
// Echo Critter v1
// Press and hold the GO/ENTER button to capture up to three seconds of audio.
// Playback always returns the recording at a playful chipmunk (~1.4x) speed.
// -----------------------------------------------------------------------------

constexpr uint32_t SAMPLE_RATE = 16000; // Microphone & speaker base rate
constexpr uint32_t DEFAULT_RECORD_MS =
    10000; // Target record length (10 seconds)
constexpr uint32_t FACE_BLINK_INTERVAL =
    200; // milliseconds between playface blinks
constexpr size_t RECORD_CHUNK_SAMPLES = 256; // Samples per I2S read block
constexpr float CHIPMUNK_MULTIPLIER = 1.4f;  // Playback rate multiplier
constexpr bool ENABLE_DEBUG_LOG = true;      // Toggle verbose serial logging

constexpr int VOLUME_STEPS = 10;       // Number of jumps between min and max
constexpr uint8_t VOLUME_MIN = 24;     // Minimum speaker volume
constexpr uint8_t VOLUME_MAX = 255;    // Maximum speaker volume
constexpr int VOLUME_PANEL_WIDTH = 36; // Reserved UI width for the volume bar

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
} // namespace Log

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
    volumeFrameDrawn = false;
    lastVolumeRatio = -1.0f;
  }

  void showSplash() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextSize(3);
    M5.Display.drawString("loopi v1", M5.Display.width() / 2,
                          M5.Display.height() / 2 - 20);
    M5.Display.setTextSize(2);
    M5.Display.drawString("GO to record", M5.Display.width() / 2,
                          M5.Display.height() / 2 + 20);
    delay(2000);
    M5.Display.fillScreen(TFT_BLACK);
    updateStatus(false, false);
    updateFace(FaceState::Idle, 0.0f);
  }

  void updateFace(FaceState state, float meter, bool blinkPhase = false) {
    if (state == lastFace && std::fabs(lastMeter - meter) < 0.02f &&
        blinkPhase == lastBlinkPhase) {
      return;
    }

    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.fillRect(VOLUME_PANEL_WIDTH, 40,
                        M5.Display.width() - VOLUME_PANEL_WIDTH, 140,
                        TFT_BLACK);

    const char *faceText = "(o_o)";
    switch (state) {
    case FaceState::Idle:
      faceText = "(o_o)";
      M5.Display.setTextSize(2);
      M5.Display.drawString("Hold GO to record", contentCenterX(), 150);
      M5.Display.fillRect(VOLUME_PANEL_WIDTH, 180,
                          M5.Display.width() - VOLUME_PANEL_WIDTH, 50,
                          TFT_BLACK);
      break;
    case FaceState::Recording:
      faceText = "(^-^)";
      drawRecordingHud(meter);
      break;
    case FaceState::Playing:
      faceText = blinkPhase ? "(^o^ )" : "( ^O^)";
      break;
    }

    M5.Display.setTextSize(4);
    M5.Display.drawString(faceText, contentCenterX(), 100);

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
    case 1:
      label = "Recording...";
      break;
    case 2:
      label = "Release to play";
      break;
    default:
      label = "Hold GO to record";
      break;
    }

    int badgeX = VOLUME_PANEL_WIDTH + 6;
    int badgeW = M5.Display.width() - (VOLUME_PANEL_WIDTH + 16);

    M5.Display.fillRoundRect(badgeX, 10, badgeW, 36, 8, TFT_DARKGREY);
    M5.Display.drawRoundRect(badgeX, 10, badgeW, 36, 8, TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::middle_left);
    M5.Display.setTextSize(2);
    M5.Display.drawString(label, badgeX + 14, 28);

    lastStatus = state;
  }

  void updateVolume(float ratio) {
    if (!volumeFrameDrawn) {
      drawVolumeFrame();
      volumeFrameDrawn = true;
    }

    ratio = constrain(ratio, 0.0f, 1.0f);
    if (std::fabs(lastVolumeRatio - ratio) < 0.01f) {
      return;
    }

    int usableHeight = volumeBarHeight - 4;
    int fillHeight = static_cast<int>(std::round(usableHeight * ratio));
    fillHeight = constrain(fillHeight, 0, usableHeight);

    M5.Display.fillRect(volumeBarX + 2, volumeBarY + 2, volumeBarWidth - 4,
                        usableHeight, TFT_BLACK);
    if (fillHeight > 0) {
      M5.Display.fillRect(volumeBarX + 2,
                          volumeBarY + 2 + (usableHeight - fillHeight),
                          volumeBarWidth - 4, fillHeight, TFT_LIGHTGREY);
    }

    lastVolumeRatio = ratio;
  }

private:
  void drawRecordingHud(float level) {
    uint16_t usableWidth = M5.Display.width() - VOLUME_PANEL_WIDTH - 40;
    uint16_t barWidth =
        static_cast<uint16_t>(usableWidth * constrain(level, 0.0f, 1.0f));
    uint16_t barHeight = 16;
    int16_t x = VOLUME_PANEL_WIDTH + 20;
    int16_t y = 190;

    M5.Display.fillRect(x, y, usableWidth, barHeight, TFT_DARKGREY);
    M5.Display.fillRect(x, y, barWidth, barHeight, TFT_GREEN);

    M5.Display.setTextDatum(textdatum_t::top_center);
    M5.Display.setTextSize(1);
    M5.Display.drawString("LISTENING", contentCenterX(), y - 14);
  }

  void drawVolumeFrame() {
    const int x = 4;
    const int topIconY = 38;

    volumeBarX = x + 10;
    volumeBarY = 60;
    volumeBarWidth = 12;
    volumeBarHeight = 128;

    M5.Display.fillRect(0, 0, VOLUME_PANEL_WIDTH, M5.Display.height(),
                        TFT_BLACK);

    M5.Display.drawRoundRect(volumeBarX, volumeBarY, volumeBarWidth,
                             volumeBarHeight, 4, TFT_DARKGREY);
    M5.Display.fillRect(volumeBarX + 1, volumeBarY + 1, volumeBarWidth - 2,
                        volumeBarHeight - 2, TFT_BLACK);

    drawSpeakerIcon(x + 8, topIconY, 3);
  }

  void drawSpeakerIcon(int cx, int cy, int waves) {
    const int width = 24;
    const int height = 16;
    const int left = cx - width / 2;
    const int top = cy - height / 2;

    M5.Display.fillRect(left, top, width, height, TFT_BLACK);

    uint16_t shellColor = TFT_DARKGREY;
    uint16_t waveColor = TFT_LIGHTGREY;

    M5.Display.fillRect(left + 2, cy - 5, 5, 10, shellColor);
    M5.Display.fillTriangle(left + 6, cy - 7, left + 12, cy, left + 6, cy + 7,
                            shellColor);

    for (int i = 0; i < waves; ++i) {
      int offset = left + 13 + i * 4;
      M5.Display.drawLine(offset, cy - 6, offset + 2, cy - 4, waveColor);
      M5.Display.drawLine(offset + 2, cy - 4, offset + 2, cy + 4, waveColor);
      M5.Display.drawLine(offset + 2, cy + 4, offset, cy + 6, waveColor);
    }
  }

  FaceState lastFace = FaceState::Idle;
  float lastMeter = -1.0f;
  bool lastBlinkPhase = false;
  int lastStatus = -1;
  bool volumeFrameDrawn = false;
  float lastVolumeRatio = -1.0f;
  int volumeBarX = 0;
  int volumeBarY = 0;
  int volumeBarWidth = 0;
  int volumeBarHeight = 0;

  int contentCenterX() const {
    return VOLUME_PANEL_WIDTH + (M5.Display.width() - VOLUME_PANEL_WIDTH) / 2;
  }
};

// -----------------------------------------------------------------------------
// Input controller
// -----------------------------------------------------------------------------

class InputController {
public:
  void begin() { keyboardEnabled = true; }

  void update() {
    M5Cardputer.update();

    const auto &keys = M5Cardputer.Keyboard.keysState();

    bool kbEnter = keyboardEnabled ? keys.enter : false;
    bool button = M5.BtnA.isPressed();
    current = kbEnter || button;
    justPressedFlag = current && !previous;
    releasedFlag = !current && previous;
    previous = current;

    bool upNow = false;
    bool downNow = false;
    if (keyboardEnabled) {
      for (uint8_t code : keys.hid_keys) {
        if (code == 0x33) {
          upNow = true;
        } else if (code == 0x37) {
          downNow = true;
        }
      }

      if (keys.fn) {
        for (char c : keys.word) {
          if (c == 'w' || c == 'W') {
            upNow = true;
          } else if (c == 's' || c == 'S') {
            downNow = true;
          }
        }
      }
    }

    upClicked = upNow && !prevUp;
    downClicked = downNow && !prevDown;
    volumeUpHeld = upNow;
    volumeDownHeld = downNow;
    prevUp = upNow;
    prevDown = downNow;
  }

  bool isPressed() const { return current; }
  bool justPressed() const { return justPressedFlag; }
  bool released() const { return releasedFlag; }
  bool volumeUpClicked() const { return upClicked; }
  bool volumeDownClicked() const { return downClicked; }
  bool volumeUpHeldNow() const { return volumeUpHeld; }
  bool volumeDownHeldNow() const { return volumeDownHeld; }

private:
  bool keyboardEnabled = false;
  bool current = false;
  bool previous = false;
  bool justPressedFlag = false;
  bool releasedFlag = false;
  bool prevUp = false;
  bool prevDown = false;
  bool upClicked = false;
  bool downClicked = false;
  bool volumeUpHeld = false;
  bool volumeDownHeld = false;
};

// -----------------------------------------------------------------------------
// Volume controller
// -----------------------------------------------------------------------------

class VolumeController {
public:
  void begin() { level = defaultInitialLevel(); }

  bool increase() {
    if (level >= VOLUME_STEPS) {
      return false;
    }
    ++level;
    return true;
  }

  bool decrease() {
    if (level == 0) {
      return false;
    }
    --level;
    return true;
  }

  uint8_t value() const {
    uint32_t range = static_cast<uint32_t>(VOLUME_MAX) - VOLUME_MIN;
    uint32_t scaled =
        (range * static_cast<uint32_t>(level) + VOLUME_STEPS / 2) /
        VOLUME_STEPS;
    return static_cast<uint8_t>(VOLUME_MIN + scaled);
  }

  float ratio() const {
    return static_cast<float>(level) / static_cast<float>(VOLUME_STEPS);
  }

private:
  int defaultInitialLevel() const {
    int suggested = (VOLUME_STEPS * 7) / 10;
    if (suggested < 0) {
      suggested = 0;
    } else if (suggested > VOLUME_STEPS) {
      suggested = VOLUME_STEPS;
    }
    return suggested;
  }

  int level = defaultInitialLevel();
};

// -----------------------------------------------------------------------------
// Audio engine
// -----------------------------------------------------------------------------

class AudioEngine {
public:
  enum class MemoryPool {
    PSRAM_DMA,
    PSRAM,
    InternalDMA,
    PSMalloc,
    Heap,
    Unknown
  };

  const char *poolName(MemoryPool pool) const {
    switch (pool) {
    case MemoryPool::PSRAM_DMA:
      return "PSRAM_DMA";
    case MemoryPool::PSRAM:
      return "PSRAM";
    case MemoryPool::InternalDMA:
      return "InternalDMA";
    case MemoryPool::PSMalloc:
      return "PSMalloc";
    case MemoryPool::Heap:
      return "Heap";
    default:
      return "Unknown";
    }
  }

  bool begin() {
    static const uint32_t candidates[] = {
        DEFAULT_RECORD_MS, 8000, 6000, 5000, 4000, 3000, 2000, 1000};

    for (uint32_t candidate : candidates) {
      size_t samples = (SAMPLE_RATE * candidate) / 1000;
      if (samples == 0) {
        continue;
      }
      size_t bytes = samples * sizeof(int16_t);

      buffer = nullptr;
      allocatedFrom = MemoryPool::Unknown;

      buffer = static_cast<int16_t *>(
          heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
      if (buffer) {
        allocatedFrom = MemoryPool::PSRAM_DMA;
      }

      if (!buffer) {
        buffer = static_cast<int16_t *>(
            heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buffer) {
          allocatedFrom = MemoryPool::PSRAM;
        }
      }
      if (!buffer) {
        buffer = static_cast<int16_t *>(
            heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_DMA));
        if (buffer) {
          allocatedFrom = MemoryPool::InternalDMA;
        }
      }
      if (!buffer) {
        buffer = static_cast<int16_t *>(ps_malloc(bytes));
        if (buffer) {
          allocatedFrom = MemoryPool::PSMalloc;
        }
      }
      if (!buffer) {
        buffer = static_cast<int16_t *>(malloc(bytes));
        if (buffer) {
          allocatedFrom = MemoryPool::Heap;
        }
      }

      if (buffer) {
        maxSamples = samples;
        maxRecordMs = candidate;
        bufferBytes = bytes;
        Log::print("Audio buffer allocation ok: %u ms (%u bytes) from %s",
                   static_cast<unsigned>(maxRecordMs),
                   static_cast<unsigned>(bufferBytes), poolName(allocatedFrom));
        break;
      }
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
               micCfg.sample_rate, micCfg.stereo,
               static_cast<unsigned>(micCfg.dma_buf_count),
               static_cast<unsigned>(micCfg.dma_buf_len));

    auto spkCfg = M5.Speaker.config();
    spkCfg.sample_rate = SAMPLE_RATE;
    spkCfg.stereo = false;
    spkCfg.dma_buf_len = 256;
    spkCfg.dma_buf_count = 6;
    M5.Speaker.config(spkCfg);
    applyVolume(currentVolume);
    Log::print("Speaker config set: rate=%u stereo=%d dma=%u/%u volume=%u",
               spkCfg.sample_rate, spkCfg.stereo,
               static_cast<unsigned>(spkCfg.dma_buf_count),
               static_cast<unsigned>(spkCfg.dma_buf_len),
               static_cast<unsigned>(M5.Speaker.getVolume()));

    M5.Speaker.end();
    bool micOk = M5.Mic.begin();
    Log::print("Mic.begin -> %s", micOk ? "ok" : "FAILED");
    return micOk;
  }

  void applyVolume(uint8_t volume) {
    if (volume > VOLUME_MAX) {
      volume = VOLUME_MAX;
    }
    if (volume < VOLUME_MIN) {
      volume = VOLUME_MIN;
    }
    if (volume != currentVolume) {
      currentVolume = volume;
      Log::print("Speaker volume set -> %u",
                 static_cast<unsigned>(currentVolume));
    }
    M5.Speaker.setVolume(currentVolume);
    M5.Speaker.setAllChannelVolume(currentVolume);
  }

  void startRecording() {
    if (!buffer) {
      return;
    }

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

    Log::print("Recording started (limit %u ms)",
               static_cast<unsigned>(maxRecordMs));
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
    Log::print("Recording complete, samples=%u",
               static_cast<unsigned>(recordedSamples));
  }

  void updateRecording() {
    if (!recording || !buffer || maxSamples == 0) {
      return;
    }

    if (recordedSamples >= maxSamples) {
      stopRecording();
      return;
    }

    size_t remaining = maxSamples - recordedSamples;
    size_t chunkSamples =
        (remaining < RECORD_CHUNK_SAMPLES) ? remaining : RECORD_CHUNK_SAMPLES;
    int16_t *dst = buffer + recordedSamples;

    if (M5.Mic.record(dst, chunkSamples, SAMPLE_RATE, false)) {
      double accum = 0.0;
      for (size_t i = 0; i < chunkSamples; ++i) {
        float s = dst[i] / 32768.0f;
        accum += s * s;
      }
      level = (chunkSamples > 0)
                  ? constrain(sqrtf(accum / static_cast<double>(chunkSamples)) *
                                  4.0f,
                              0.0f, 1.0f)
                  : 0.0f;
      recordedSamples += chunkSamples;
      levelTimestamp = millis();
      hasRecordingFlag = recordedSamples > 0;
      Log::print("Captured chunk=%u total=%u meter=%.2f",
                 static_cast<unsigned>(chunkSamples),
                 static_cast<unsigned>(recordedSamples), level);
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
    applyVolume(currentVolume);

    uint32_t playbackRate =
        static_cast<uint32_t>(SAMPLE_RATE * CHIPMUNK_MULTIPLIER);
    bool started =
        M5.Speaker.playRaw(buffer, recordedSamples, playbackRate, false, 1, 0);
    Log::print("Playback started samples=%u",
               static_cast<unsigned>(recordedSamples));

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
  uint32_t recordLimitMs() const { return maxRecordMs; }
  uint32_t recordingElapsed() const {
    return recording ? millis() - recordStart : 0;
  }

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
  size_t maxSamples = 0;
  uint32_t maxRecordMs = DEFAULT_RECORD_MS;
  size_t bufferBytes = 0;
  MemoryPool allocatedFrom = MemoryPool::Unknown;
  uint8_t currentVolume = VOLUME_MAX;
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
      while (true) {
        delay(1000);
      }
    }

    volume.begin();
    audio.applyVolume(volume.value());
    display.updateVolume(volume.ratio());
  }

  void loop() {
    input.update();

    bool volumeAdjusted = false;
    if (input.volumeUpClicked()) {
      if (volume.increase()) {
        volumeAdjusted = true;
      }
    }
    if (input.volumeDownClicked()) {
      if (volume.decrease()) {
        volumeAdjusted = true;
      }
    }
    if (volumeAdjusted) {
      audio.applyVolume(volume.value());
      display.updateVolume(volume.ratio());
    }

    if (input.justPressed() && !audio.isRecording()) {
      audio.startRecording();
      display.updateStatus(true, false);
      display.updateFace(FaceState::Recording, 0.0f);
    }

    if (audio.isRecording()) {
      audio.updateRecording();
      display.updateFace(FaceState::Recording, audio.currentLevel());
      display.updateStatus(true, audio.hasRecording());
      if (audio.recordingElapsed() >= audio.recordLimitMs()) {
        audio.stopRecording();
        Log::print("Max duration reached -> auto playback");
        autoPlayPending = true;
      }
    }

    if (input.released()) {
      Log::print(
          "Enter released, recordingActive=%d hasRecording=%d samples=%u",
          audio.isRecording() ? 1 : 0, audio.hasRecording() ? 1 : 0,
          static_cast<unsigned>(audio.sampleCount()));

      if (audio.isRecording()) {
        audio.stopRecording();
      }

      if (audio.hasRecording()) {
        display.updateStatus(false, true);
        display.updateFace(FaceState::Playing, 0.0f);
        if (audio.playChipmunk(display)) {
          audio.resetAfterPlayback();
          autoPlayPending = false;
        }
      } else {
        display.updateFace(FaceState::Idle, 0.0f);
        display.updateStatus(false, false);
      }
    }

    if (autoPlayPending && audio.hasRecording() && !audio.isRecording()) {
      display.updateStatus(false, true);
      display.updateFace(FaceState::Playing, 0.0f);
      if (audio.playChipmunk(display)) {
        audio.resetAfterPlayback();
        autoPlayPending = false;
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
  VolumeController volume;
  bool autoPlayPending = false;
};

// -----------------------------------------------------------------------------
// Arduino hooks
// -----------------------------------------------------------------------------

EchoApp app;

void setup() { app.setup(); }

void loop() { app.loop(); }
