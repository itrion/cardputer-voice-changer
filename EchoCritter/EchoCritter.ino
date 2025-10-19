#include <M5Cardputer.h>

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
constexpr bool ENABLE_DEBUG_LOG = true;          // Toggle verbose serial logging

// Visual state for the face renderer.
enum FaceState { FACE_IDLE, FACE_RECORDING, FACE_PLAYING };

int16_t *audioBuffer = nullptr;                   // Sample storage in (P)SRAM
size_t recordedSamples = 0;                       // Number of valid samples
bool hasRecording = false;                        // True when recording is available

bool recordingActive = false;                     // True while capturing audio
uint32_t recordStartMs = 0;                       // Timestamp when recording started

uint32_t blinkTimer = 0;                          // Used for playback face animation
bool blinkPhase = false;                          // Toggle for blinking animation

float micLastMeter = 0.0f;
uint32_t micLastMeterUpdate = 0;

bool cardputerKeyboardEnabled = false;

void logf(const char *fmt, ...) {
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

// Forward declarations for helper functions requested in the task description.
void initDisplay();
void initAudio();
void recordOnce(float &levelOut);
void playChipmunk();
void drawFace(FaceState state, float meter = 0.0f);
void drawStatusBadge();

// Utility helpers.
void showSplash();
void updateInput();
void stopRecording();
void startRecording();
void drawRecordingHud(float level);
void playBuffer(uint32_t playbackRate);

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
  cardputerKeyboardEnabled = true;
  logf("Booting Echo Critter, board=%d", static_cast<int>(M5.getBoard()));
  logf("Cardputer keyboard ready");

  // Allocate audio buffer preferably in PSRAM.
  audioBuffer = static_cast<int16_t *>(ps_malloc(BUFFER_BYTES));
  if (!audioBuffer) {
    audioBuffer = static_cast<int16_t *>(malloc(BUFFER_BYTES));
  }
  logf("Audio buffer allocation %s (%u bytes)", audioBuffer ? "ok" : "FAILED", static_cast<unsigned>(BUFFER_BYTES));

  initDisplay();
  initAudio();
  showSplash();
}

void loop() {
  updateInput();

  bool enterPressed = false;
  bool enterJustPressed = false;
  bool enterReleased = false;

  bool goPressed = M5.BtnA.isPressed();
  bool goJustPressed = M5.BtnA.wasPressed();
  bool goReleased = M5.BtnA.wasReleased();

  if (cardputerKeyboardEnabled) {
    static bool prevEnter = false;
    bool enterNow = M5Cardputer.Keyboard.keysState().enter;
    enterPressed = enterNow || goPressed;
    enterJustPressed = (enterNow && !prevEnter) || goJustPressed;
    enterReleased = (!enterNow && prevEnter) || goReleased;
    prevEnter = enterNow;
  } else {
    enterPressed = goPressed;
    enterJustPressed = goJustPressed;
    enterReleased = goReleased;
  }

  if (enterJustPressed && !recordingActive) {
    startRecording();
  }

  float rmsLevel = 0.0f;
  if (recordingActive) {
    recordOnce(rmsLevel);
    drawFace(FACE_RECORDING, rmsLevel);
    drawStatusBadge();
    if (millis() - recordStartMs >= MAX_RECORD_MS) {
      logf("Max record duration reached");
      stopRecording();
    }
  }

  if (enterReleased) {
    logf("Enter released, recordingActive=%d hasRecording=%d samples=%u", recordingActive ? 1 : 0, hasRecording ? 1 : 0, static_cast<unsigned>(recordedSamples));
    if (recordingActive) {
      stopRecording();
    }

    if (hasRecording && recordedSamples > 0) {
      logf("Playback chipmunk mode");
      playChipmunk();
      hasRecording = false;
      recordedSamples = 0;
      micLastMeter = 0.0f;
      micLastMeterUpdate = millis();
    } else {
      logf("No recording available on release");
    }

    drawStatusBadge();
    drawFace(FACE_IDLE, 0.0f);
  }

  if (!recordingActive && !enterPressed && !hasRecording) {
    // Idle prompt when nothing has been captured yet.
    drawFace(FACE_IDLE, 0.0f);
    drawStatusBadge();
  }
}

// -----------------------------------------------------------------------------
// Display helpers
// -----------------------------------------------------------------------------

void initDisplay() {
  M5.Display.setRotation(1);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.fillScreen(TFT_BLACK);
}

void drawFace(FaceState state, float meter) {
  static FaceState lastState = static_cast<FaceState>(-1);
  static int lastBlink = -1;
  static float lastMeter = -1.0f;

  // Only redraw when something changes to reduce flicker.
  bool needsRefresh = (state != lastState);
  if (!needsRefresh && state == FACE_RECORDING) {
    needsRefresh = fabsf(lastMeter - meter) > 0.02f;
  }
  if (!needsRefresh && state == FACE_PLAYING) {
    int phase = blinkPhase ? 1 : 0;
    if (phase != lastBlink) {
      needsRefresh = true;
      lastBlink = phase;
    }
  }

  if (!needsRefresh) {
    return;
  }

  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.fillRect(0, 40, M5.Display.width(), 140, TFT_BLACK);

  const char *faceText = "(o_o)";
  switch (state) {
    case FACE_IDLE:
      faceText = "(o_o)";
      M5.Display.setTextSize(2);
      M5.Display.drawString("Hold GO to record", M5.Display.width() / 2, 150);
      M5.Display.fillRect(0, 180, M5.Display.width(), 50, TFT_BLACK);
      break;
    case FACE_RECORDING:
      faceText = "(^-^)";
      drawRecordingHud(meter);
      break;
    case FACE_PLAYING:
      faceText = blinkPhase ? "(^o^)" : "(^O^)";
      break;
  }

  M5.Display.setTextSize(4);
  M5.Display.drawString(faceText, M5.Display.width() / 2, 100);

  lastState = state;
  lastMeter = meter;
  if (state != FACE_PLAYING) {
    lastBlink = -1;
  }
}

void drawStatusBadge() {
  static int lastState = -1;
  int state = recordingActive ? 1 : (hasRecording ? 2 : 0);
  if (state == lastState) {
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

  lastState = state;
}

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

void updateInput() {
  if (cardputerKeyboardEnabled) {
    M5Cardputer.update();
  } else {
    M5.update();
  }
}

// -----------------------------------------------------------------------------
// Audio initialisation
// -----------------------------------------------------------------------------

void initAudio() {
  if (!audioBuffer) {
    M5.Display.setTextDatum(textdatum_t::top_center);
    M5.Display.setTextSize(2);
    M5.Display.drawString("Audio buffer alloc failed!", M5.Display.width() / 2, 0);
    logf("FATAL: audioBuffer allocation failed");
    while (true) {
      delay(1000);
    }
  }

  auto micCfg = M5.Mic.config();
  micCfg.sample_rate = SAMPLE_RATE;
  micCfg.stereo = false;
  micCfg.dma_buf_count = 6;
  micCfg.dma_buf_len = 256;
  M5.Mic.config(micCfg);
  logf("Mic config set: rate=%u stereo=%d dma=%u/%u", micCfg.sample_rate, micCfg.stereo, static_cast<unsigned>(micCfg.dma_buf_count), static_cast<unsigned>(micCfg.dma_buf_len));

  auto spkCfg = M5.Speaker.config();
  spkCfg.sample_rate = SAMPLE_RATE;
  spkCfg.stereo = false;
  spkCfg.dma_buf_len = 256;
  spkCfg.dma_buf_count = 6;
  M5.Speaker.config(spkCfg);
  M5.Speaker.setVolume(180);
  logf("Speaker config set: rate=%u stereo=%d dma=%u/%u volume=%u", spkCfg.sample_rate, spkCfg.stereo, static_cast<unsigned>(spkCfg.dma_buf_count), static_cast<unsigned>(spkCfg.dma_buf_len), static_cast<unsigned>(M5.Speaker.getVolume()));

  M5.Speaker.end();  // Free I2S for the microphone by default
  bool micOk = M5.Mic.begin();
  logf("Mic.begin -> %s", micOk ? "ok" : "FAILED");
}

// -----------------------------------------------------------------------------
// Recording logic
// -----------------------------------------------------------------------------

void startRecording() {
  if (!audioBuffer) {
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
  hasRecording = false;
  recordingActive = true;
  recordStartMs = millis();
  blinkTimer = recordStartMs;
  blinkPhase = false;
  micLastMeter = 0.0f;
  micLastMeterUpdate = recordStartMs;

  logf("Recording started");
  drawFace(FACE_RECORDING, 0.0f);
  drawStatusBadge();
}

void stopRecording() {
  if (!recordingActive) {
    return;
  }

  recordingActive = false;
  while (M5.Mic.isRecording() != 0) {
    delay(2);
  }

  hasRecording = recordedSamples > 0;
  logf("Recording complete, samples=%u", static_cast<unsigned>(recordedSamples));
}

void recordOnce(float &levelOut) {
  levelOut = micLastMeter;

  if (!recordingActive || !audioBuffer) {
    levelOut = 0.0f;
    return;
  }

  if (recordedSamples >= MAX_SAMPLES) {
    stopRecording();
    return;
  }

  size_t remaining = MAX_SAMPLES - recordedSamples;
  size_t chunkSamples = (remaining < RECORD_CHUNK_SAMPLES) ? remaining : RECORD_CHUNK_SAMPLES;
  int16_t *dst = audioBuffer + recordedSamples;

  if (M5.Mic.record(dst, chunkSamples, SAMPLE_RATE, false)) {
    double accum = 0.0;
    for (size_t i = 0; i < chunkSamples; ++i) {
      float s = dst[i] / 32768.0f;
      accum += s * s;
    }
    micLastMeter = (chunkSamples > 0)
        ? constrain(sqrtf(accum / static_cast<double>(chunkSamples)) * 4.0f, 0.0f, 1.0f)
        : 0.0f;
    recordedSamples += chunkSamples;
    micLastMeterUpdate = millis();
    hasRecording = recordedSamples > 0;
    logf("Captured chunk=%u total=%u meter=%.2f", static_cast<unsigned>(chunkSamples), static_cast<unsigned>(recordedSamples), micLastMeter);
  } else if (millis() - micLastMeterUpdate > 60) {
    micLastMeter *= 0.92f;
    if (micLastMeter < 0.01f) {
      micLastMeter = 0.0f;
    }
    micLastMeterUpdate = millis();
  }

  levelOut = micLastMeter;
}

// -----------------------------------------------------------------------------
// Playback logic
// -----------------------------------------------------------------------------

void playChipmunk() {
  uint32_t rate = static_cast<uint32_t>(SAMPLE_RATE * 1.4f);
  playBuffer(rate);
}

void playBuffer(uint32_t playbackRate) {
  if (!audioBuffer || !hasRecording || recordedSamples == 0) {
    logf("playBuffer aborted: buffer=%d hasRecording=%d samples=%u", audioBuffer != nullptr, hasRecording ? 1 : 0, static_cast<unsigned>(recordedSamples));
    return;
  }

  if (M5.Mic.isRunning()) {
    logf("Stopping mic before playback");
    while (M5.Mic.isRecording() != 0) {
      delay(2);
    }
    M5.Mic.end();
  }

  if (M5.Speaker.isRunning()) {
    M5.Speaker.stop();
    M5.Speaker.end();
  }

  auto spkCfg = M5.Speaker.config();
  spkCfg.sample_rate = playbackRate;
  M5.Speaker.config(spkCfg);
  M5.Speaker.begin();
  logf("Speaker begin at rate=%u", playbackRate);

  blinkTimer = millis();
  blinkPhase = false;
  drawFace(FACE_PLAYING, 0.0f);
  drawStatusBadge();

  M5.Speaker.playRaw(audioBuffer, recordedSamples, playbackRate, false, 1, 0);
  logf("Playback started samples=%u", static_cast<unsigned>(recordedSamples));

  while (M5.Speaker.isPlaying()) {
    if (millis() - blinkTimer > FACE_BLINK_INTERVAL) {
      blinkTimer = millis();
      blinkPhase = !blinkPhase;
      drawFace(FACE_PLAYING, 0.0f);
    }
    delay(10);
    updateInput();
  }

  // Ensure audio hardware stops when finished.
  M5.Speaker.stop();
  M5.Speaker.end();
  M5.Mic.begin();
  logf("Playback finished, mic resumed");
  blinkPhase = false;
}

// -----------------------------------------------------------------------------
// Misc helpers
// -----------------------------------------------------------------------------

void showSplash() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setTextSize(3);
  M5.Display.drawString("Echo Critter v1", M5.Display.width() / 2, M5.Display.height() / 2 - 20);
  M5.Display.setTextSize(2);
  M5.Display.drawString("Hold GO to record", M5.Display.width() / 2, M5.Display.height() / 2 + 20);
  delay(2000);
  M5.Display.fillScreen(TFT_BLACK);
  drawStatusBadge();
  drawFace(FACE_IDLE, 0.0f);
}
