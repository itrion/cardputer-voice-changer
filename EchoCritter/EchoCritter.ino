#include <M5Unified.h>
#include <M5GFX.h>
#include <esp32-hal-psram.h>
#include <cmath>
#include <cstring>

// -----------------------------------------------------------------------------
// Echo Critter v1
// Press and hold the ENTER button to capture up to three seconds of audio.
// Playback automatically alternates between NORMAL (1.0x) and MOUSE (~1.4x).
// -----------------------------------------------------------------------------

constexpr uint32_t SAMPLE_RATE = 16000;           // Microphone & speaker base rate
constexpr uint32_t MAX_RECORD_MS = 3000;          // Maximum record length (3 seconds)
constexpr size_t MAX_SAMPLES = SAMPLE_RATE * MAX_RECORD_MS / 1000;
constexpr size_t BUFFER_BYTES = MAX_SAMPLES * sizeof(int16_t);
constexpr uint32_t FACE_BLINK_INTERVAL = 120;     // milliseconds between playface blinks

// Modes for playback behaviour.
enum PlaybackMode { MODE_NORMAL = 0, MODE_MOUSE = 1 };

// Visual state for the face renderer.
enum FaceState { FACE_IDLE, FACE_RECORDING, FACE_PLAYING };

int16_t *audioBuffer = nullptr;                   // Sample storage in (P)SRAM
size_t recordedSamples = 0;                       // Number of valid samples
bool hasRecording = false;                        // True when recording is available
PlaybackMode currentMode = MODE_NORMAL;           // Alternates every playback

bool recordingActive = false;                     // True while capturing audio
uint32_t recordStartMs = 0;                       // Timestamp when recording started

uint32_t blinkTimer = 0;                          // Used for playback face animation
bool blinkPhase = false;                          // Toggle for blinking animation

// Forward declarations for helper functions requested in the task description.
void initDisplay();
void initAudio();
void recordOnce(float &levelOut);
void playNormal();
void playMouse();
void drawFace(FaceState state, float meter = 0.0f);
void drawModeBadge();

// Utility helpers.
void showSplash();
void stopRecording();
void startRecording();
void drawRecordingHud(float level);
void playBuffer(uint32_t playbackRate);

void setup() {
  auto cfg = M5.config();
  cfg.output_power = true;        // Ensure speaker amplifier is powered
  cfg.internal_mic = true;        // Enable internal microphone path
  cfg.external_speaker = true;    // Speaker is connected via I2S
  M5.begin(cfg);

  // Allocate audio buffer preferably in PSRAM.
  audioBuffer = static_cast<int16_t *>(ps_malloc(BUFFER_BYTES));
  if (!audioBuffer) {
    audioBuffer = static_cast<int16_t *>(malloc(BUFFER_BYTES));
  }

  initDisplay();
  initAudio();
  showSplash();
}

void loop() {
  M5.update();

  // ENTER button on Cardputer maps to BtnA in M5Unified.
  bool enterPressed = M5.BtnA.isPressed();
  bool enterJustPressed = M5.BtnA.wasPressed();
  bool enterReleased = M5.BtnA.wasReleased();

  if (enterJustPressed && !recordingActive) {
    startRecording();
  }

  float rmsLevel = 0.0f;
  if (recordingActive) {
    recordOnce(rmsLevel);
    drawFace(FACE_RECORDING, rmsLevel);
    drawModeBadge();
    if (millis() - recordStartMs >= MAX_RECORD_MS) {
      stopRecording();
    }
  }

  if (enterReleased) {
    if (recordingActive) {
      stopRecording();
    }

    if (hasRecording && recordedSamples > 0) {
      if (currentMode == MODE_NORMAL) {
        playNormal();
        currentMode = MODE_MOUSE;
      } else {
        playMouse();
        currentMode = MODE_NORMAL;
      }
    }

    drawModeBadge();
    drawFace(FACE_IDLE, 0.0f);
  }

  if (!recordingActive && !enterPressed && !hasRecording) {
    // Idle prompt when nothing has been captured yet.
    drawFace(FACE_IDLE, 0.0f);
    drawModeBadge();
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
      M5.Display.drawString("Hold ENTER to record", M5.Display.width() / 2, 150);
      M5.Display.fillRect(0, 180, M5.Display.width(), 50, TFT_BLACK);
      break;
    case FACE_RECORDING:
      faceText = "(^-^)";
      drawRecordingHud(meter);
      break;
    case FACE_PLAYING:
      faceText = blinkPhase ? "(*>w<)" : "(*>w<)♪";
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

void drawModeBadge() {
  static PlaybackMode lastMode = static_cast<PlaybackMode>(-1);
  if (lastMode == currentMode) {
    return;
  }

  const char *label = (currentMode == MODE_NORMAL) ? "Mode: NORMAL" : "Mode: MOUSE";

  M5.Display.fillRoundRect(10, 10, 200, 36, 8, TFT_DARKGREY);
  M5.Display.drawRoundRect(10, 10, 200, 36, 8, TFT_WHITE);
  M5.Display.setTextDatum(textdatum_t::middle_left);
  M5.Display.setTextSize(2);
  M5.Display.drawString(label, 24, 28);

  lastMode = currentMode;
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

// -----------------------------------------------------------------------------
// Audio initialisation
// -----------------------------------------------------------------------------

void initAudio() {
  if (!audioBuffer) {
    M5.Display.setTextDatum(textdatum_t::top_center);
    M5.Display.setTextSize(2);
    M5.Display.drawString("Audio buffer alloc failed!", M5.Display.width() / 2, 0);
    while (true) {
      delay(1000);
    }
  }

  auto micCfg = M5.Mic.config();
  micCfg.sample_rate = SAMPLE_RATE;
  micCfg.stereo = false;
  micCfg.dma_buf_count = 6;
  micCfg.dma_buf_len = 256;
  M5.Mic.begin(micCfg);

  auto spkCfg = M5.Speaker.config();
  spkCfg.sample_rate = SAMPLE_RATE;
  spkCfg.stereo = false;
  spkCfg.dma_buf_len = 256;
  spkCfg.dma_buf_count = 6;
  M5.Speaker.begin(spkCfg);
  M5.Speaker.setVolume(128);
}

// -----------------------------------------------------------------------------
// Recording logic
// -----------------------------------------------------------------------------

void startRecording() {
  if (!audioBuffer) {
    return;
  }

  recordedSamples = 0;
  hasRecording = false;
  recordingActive = true;
  recordStartMs = millis();
  blinkTimer = millis();
  blinkPhase = false;

  M5.Mic.start();
  drawFace(FACE_RECORDING, 0.0f);
  drawModeBadge();
}

void stopRecording() {
  M5.Mic.stop();
  recordingActive = false;
  hasRecording = recordedSamples > 0;
}

void recordOnce(float &levelOut) {
  if (!recordingActive) {
    levelOut = 0.0f;
    return;
  }

  const size_t chunkSamples = 256;
  int16_t temp[chunkSamples];

  size_t samples = M5.Mic.record(temp, chunkSamples);
  if (samples == 0) {
    levelOut = 0.0f;
    return;
  }

  size_t remaining = MAX_SAMPLES - recordedSamples;
  if (samples > remaining) {
    samples = remaining;
  }

  memcpy(audioBuffer + recordedSamples, temp, samples * sizeof(int16_t));
  recordedSamples += samples;

  if (recordedSamples >= MAX_SAMPLES) {
    stopRecording();
  }

  // Calculate a simple RMS level for the visual meter.
  double accum = 0.0;
  for (size_t i = 0; i < samples; ++i) {
    const float s = temp[i] / 32768.0f;
    accum += s * s;
  }
  float rms = (samples > 0) ? sqrtf(accum / samples) : 0.0f;
  levelOut = constrain(rms * 4.0f, 0.0f, 1.0f);
}

// -----------------------------------------------------------------------------
// Playback logic
// -----------------------------------------------------------------------------

void playNormal() {
  playBuffer(SAMPLE_RATE);
}

void playMouse() {
  uint32_t rate = static_cast<uint32_t>(SAMPLE_RATE * 1.4f);
  playBuffer(rate);
}

void playBuffer(uint32_t playbackRate) {
  if (!audioBuffer || !hasRecording || recordedSamples == 0) {
    return;
  }

  auto spkCfg = M5.Speaker.config();
  spkCfg.sample_rate = playbackRate;
  M5.Speaker.begin(spkCfg);

  blinkTimer = millis();
  blinkPhase = false;
  drawFace(FACE_PLAYING, 0.0f);
  drawModeBadge();

  const size_t chunkSamples = 256;
  size_t offset = 0;
  while (offset < recordedSamples) {
    size_t remaining = recordedSamples - offset;
    size_t toWrite = (remaining < chunkSamples) ? remaining : chunkSamples;

    M5.Speaker.playRaw(audioBuffer + offset, toWrite, playbackRate);
    offset += toWrite;

    // Blink animation timing.
    if (millis() - blinkTimer > FACE_BLINK_INTERVAL) {
      blinkTimer = millis();
      blinkPhase = !blinkPhase;
      drawFace(FACE_PLAYING, 0.0f);
    }
  }

  // Ensure audio hardware stops when finished.
  M5.Speaker.stop();
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
  M5.Display.drawString("Hold ENTER to record", M5.Display.width() / 2, M5.Display.height() / 2 + 20);
  delay(2000);
  M5.Display.fillScreen(TFT_BLACK);
  drawModeBadge();
  drawFace(FACE_IDLE, 0.0f);
}
