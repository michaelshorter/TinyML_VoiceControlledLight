
/* VoiceLamp
 * Voice-controlled 8x8 NeoPixel matrix lamp, driven by an Edge Impulse
 * keyword-spotting model running on an Arduino Nano 33 BLE Sense.
 *
 * Commands: "light", "dim", "off", "disco"
 * - light/dim/off control the lit area (shape) and brightness
 * - disco switches the matrix to a white sparkle until another
 *   command arrives
 *
 * Built on top of the Edge Impulse exported Arduino library example
 * "nano_ble33_sense_microphone_continuous". Sections marked LAMP are
 * additions on top of that base; everything else is the original
 * Edge Impulse SDK plumbing, left unchanged.
 *
 * See README.md for wiring, required libraries, and build notes.
 */

#define EIDSP_QUANTIZE_FILTERBANK   0
// 1000 ms window / 125 ms stride = 8 slices. Keep this in step with your
// Studio impulse settings.
#define EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW 8

/* Includes ---------------------------------------------------------------- */
#include <PDM.h>
#include <VoiceLamp_JandM_inferencing.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

/** Audio buffers, pointers and selectors */
typedef struct {
    signed short *buffers[2];
    unsigned char buf_select;
    unsigned char buf_ready;
    unsigned int buf_count;
    unsigned int n_samples;
} inference_t;

static inference_t inference;
static bool record_ready = false;
static signed short *sampleBuffer;
static bool debug_nn = false;
static int print_results = -(EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW);

// ==== LAMP: calibration logging ====
// 1 = compact log for threshold tuning (command scores >= LOG_MIN_SCORE, plus
//     a TRIGGER line whenever the lamp acts on a command).
// 0 = original verbose prediction printout.
#define CALIBRATION_LOG 1
const float LOG_MIN_SCORE = 0.20;

// ==== LAMP: NeoMatrix setup ====
#define LED_PIN 6
Adafruit_NeoMatrix matrix(8, 8, LED_PIN,
  NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_ROWS + NEO_MATRIX_ZIGZAG,
  NEO_GRB + NEO_KHZ800);

const float CENTER_X = 3.5, CENTER_Y = 3.5;

// Thresholds calibrated from the first ten-takes run. Recalibrate after retraining.
struct LampTarget { float radius; uint8_t brightness; uint8_t r, g, b; float threshold; };
LampTarget TARGET_LIGHT = {5.5, 60, 255, 200, 120, 0.80};
LampTarget TARGET_DIM   = {2.2, 25, 255, 200, 120, 0.70};
LampTarget TARGET_OFF   = {0.0, 0,  0,   0,   0,   0.50};
const float DISCO_THRESHOLD = 0.70;
const uint8_t DISCO_CONFIRM_SLICES = 2;   // consecutive slices needed to trigger disco
uint8_t discoStreak = 0;

float currentRadius = 0;
float currentBrightness = 0;
LampTarget activeTarget = TARGET_OFF;

unsigned long lastCommandTime = 0;
const unsigned long COOLDOWN_MS = 800;
const float EASE_FACTOR = 0.5;  // fraction of remaining distance covered per frame

// ==== LAMP: disco sparkle ====
bool discoMode = false;
float sparkle[8][8];             // current intensity 0..1 per pixel
const float SPARKLE_DECAY = 0.70;           // lower = shorter trails
const uint8_t SPARKLE_NEW_PER_FRAME = 5;    // higher = busier sparkle
const uint8_t DISCO_BRIGHTNESS = 60;

void sparkleStep() {
  matrix.setBrightness(DISCO_BRIGHTNESS);

  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) sparkle[y][x] *= SPARKLE_DECAY;

  for (uint8_t i = 0; i < SPARKLE_NEW_PER_FRAME; i++) {
    int x = random(8), y = random(8);
    sparkle[y][x] = 0.5 + random(51) / 100.0;   // peak between 0.5 and 1.0 for a twinkle
  }

  matrix.fillScreen(0);
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      float v = sparkle[y][x];
      if (v < 0.03) continue;
      uint8_t w = (uint8_t)(255 * v);
      matrix.drawPixel(x, y, matrix.Color(w, w, w));
    }
  }
  matrix.show();
}

// ==== LAMP: logging helpers ====
void logScores(const ei_impulse_result_t &result) {
#if CALIBRATION_LOG
  bool any = false;
  for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
    const char* label = result.classification[ix].label;
    if (strcmp(label, "background") == 0 || strcmp(label, "unknown") == 0) continue;
    float v = result.classification[ix].value;
    if (v >= LOG_MIN_SCORE) {
      if (!any) { Serial.print(millis()); any = true; }
      Serial.print(" ");
      Serial.print(label);
      Serial.print("=");
      Serial.print(v, 2);
    }
  }
  if (any) Serial.println();
#endif
}

void logTrigger(const char* label, float confidence) {
#if CALIBRATION_LOG
  Serial.print("TRIGGER ");
  Serial.print(label);
  Serial.print(" ");
  Serial.println(confidence, 2);
#endif
}

void setLampCommand(const char* label, float confidence) {
  // Count consecutive slices where disco clears its threshold. Runs before
  // the cooldown check so the streak keeps counting during cooldown.
  if (strcmp(label, "disco") == 0 && confidence >= DISCO_THRESHOLD) discoStreak++;
  else discoStreak = 0;

  if (millis() - lastCommandTime < COOLDOWN_MS) return;

  if (discoStreak >= DISCO_CONFIRM_SLICES) {
    discoStreak = 0;
    discoMode = true;
    currentRadius = 0;       // so light/dim grow from nothing when disco ends
    currentBrightness = 0;
    lastCommandTime = millis();
    logTrigger(label, confidence);
    return;
  }

  LampTarget* chosen = nullptr;
  if (strcmp(label, "light") == 0 && confidence >= TARGET_LIGHT.threshold) chosen = &TARGET_LIGHT;
  else if (strcmp(label, "dim") == 0 && confidence >= TARGET_DIM.threshold) chosen = &TARGET_DIM;
  else if (strcmp(label, "off") == 0 && confidence >= TARGET_OFF.threshold) chosen = &TARGET_OFF;

  if (chosen == nullptr) return; // didn't meet its own threshold, or wasn't a command at all

  discoMode = false;
  activeTarget = *chosen;
  lastCommandTime = millis();
  logTrigger(label, confidence);
}

void updateLampAnimation() {
  if (discoMode) {
    sparkleStep();
    return;
  }

  currentRadius += (activeTarget.radius - currentRadius) * EASE_FACTOR;
  currentBrightness += (activeTarget.brightness - currentBrightness) * EASE_FACTOR;

  matrix.setBrightness((uint8_t)currentBrightness);
  matrix.fillScreen(0);
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      float dx = x - CENTER_X, dy = y - CENTER_Y;
      float dist = sqrt(dx * dx + dy * dy);
      float edge = currentRadius - dist;
      if (edge > 0) {
        float intensity = constrain(edge, 0.0, 1.0);
        matrix.drawPixel(x, y, matrix.Color(
          activeTarget.r * intensity, activeTarget.g * intensity, activeTarget.b * intensity));
      }
    }
  }
  matrix.show();
}
// ==== END LAMP ====

void setup()
{
    Serial.begin(115200);
    // Wait up to 3 s for the Serial Monitor, then carry on so the lamp
    // also runs when powered without a computer.
    unsigned long serialStart = millis();
    while (!Serial && millis() - serialStart < 3000) { }
    Serial.println("Edge Impulse Inferencing Demo");

    ei_printf("Inferencing settings:\n");
    ei_printf("\tInterval: %.2f ms.\n", (float)EI_CLASSIFIER_INTERVAL_MS);
    ei_printf("\tFrame size: %d\n", EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
    ei_printf("\tSample length: %d ms.\n", EI_CLASSIFIER_RAW_SAMPLE_COUNT / 16);
    ei_printf("\tNo. of classes: %d\n", sizeof(ei_classifier_inferencing_categories) /
                                            sizeof(ei_classifier_inferencing_categories[0]));

    run_classifier_init();
    if (microphone_inference_start(EI_CLASSIFIER_SLICE_SIZE) == false) {
        ei_printf("ERR: Could not allocate audio buffer (size %d), this could be due to the window length of your model\r\n", EI_CLASSIFIER_RAW_SAMPLE_COUNT);
        return;
    }

    // ==== LAMP: init matrix ====
    randomSeed(analogRead(A0));
    matrix.begin();
    matrix.fillScreen(0);
    matrix.show();
}

void loop()
{
    bool m = microphone_inference_record();
    if (!m) {
        ei_printf("ERR: Failed to record audio...\n");
        return;
    }

    signal_t signal;
    signal.total_length = EI_CLASSIFIER_SLICE_SIZE;
    signal.get_data = &microphone_audio_signal_get_data;
    ei_impulse_result_t result = {0};

    EI_IMPULSE_ERROR r = run_classifier_continuous(&signal, &result, debug_nn);
    if (r != EI_IMPULSE_OK) {
        ei_printf("ERR: Failed to run classifier (%d)\n", r);
        return;
    }

    // ==== LAMP: compact score log (calibration mode only) ====
    logScores(result);

    // ==== LAMP: find best-scoring class and pass it to the command handler ====
    size_t best_ix = 0;
    for (size_t ix = 1; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        if (result.classification[ix].value > result.classification[best_ix].value) {
            best_ix = ix;
        }
    }
    setLampCommand(result.classification[best_ix].label, result.classification[best_ix].value);

#if !CALIBRATION_LOG
    if (++print_results >= (EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW)) {
        ei_printf("Predictions ");
        ei_printf("(DSP: %d ms., Classification: %d ms., Anomaly: %d ms.)",
            result.timing.dsp, result.timing.classification, result.timing.anomaly);
        ei_printf(": \n");
        for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
            ei_printf("    %s: %.5f\n", result.classification[ix].label,
                      result.classification[ix].value);
        }
#if EI_CLASSIFIER_HAS_ANOMALY == 1
        ei_printf("    anomaly score: %.3f\n", result.anomaly);
#endif
        print_results = 0;
    }
#endif

    // ==== LAMP: step the animation every loop, regardless of print throttle ====
    updateLampAnimation();
}

// ---- everything below here is UNCHANGED from the original exported file ----

static void pdm_data_ready_inference_callback(void)
{
    int bytesAvailable = PDM.available();
    int bytesRead = PDM.read((char *)&sampleBuffer[0], bytesAvailable);

    if (record_ready == true) {
        for (int i = 0; i<bytesRead>>1; i++) {
            inference.buffers[inference.buf_select][inference.buf_count++] = sampleBuffer[i];

            if (inference.buf_count >= inference.n_samples) {
                inference.buf_select ^= 1;
                inference.buf_count = 0;
                inference.buf_ready = 1;
            }
        }
    }
}

static bool microphone_inference_start(uint32_t n_samples)
{
    inference.buffers[0] = (signed short *)malloc(n_samples * sizeof(signed short));
    if (inference.buffers[0] == NULL) return false;

    inference.buffers[1] = (signed short *)malloc(n_samples * sizeof(signed short));
    if (inference.buffers[1] == NULL) {
        free(inference.buffers[0]);
        return false;
    }

    sampleBuffer = (signed short *)malloc((n_samples >> 1) * sizeof(signed short));
    if (sampleBuffer == NULL) {
        free(inference.buffers[0]);
        free(inference.buffers[1]);
        return false;
    }

    inference.buf_select = 0;
    inference.buf_count = 0;
    inference.n_samples = n_samples;
    inference.buf_ready = 0;

    PDM.onReceive(&pdm_data_ready_inference_callback);
    PDM.setBufferSize((n_samples >> 1) * sizeof(int16_t));

    if (!PDM.begin(1, EI_CLASSIFIER_FREQUENCY)) {
        ei_printf("Failed to start PDM!");
    }

    PDM.setGain(127);
    record_ready = true;

    return true;
}

static bool microphone_inference_record(void)
{
    bool ret = true;

    if (inference.buf_ready == 1) {
        ei_printf(
            "Error sample buffer overrun. Decrease the number of slices per model window "
            "(EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW)\n");
        ret = false;
    }

    while (inference.buf_ready == 0) {
        delay(1);
    }

    inference.buf_ready = 0;

    return ret;
}

static int microphone_audio_signal_get_data(size_t offset, size_t length, float *out_ptr)
{
    numpy::int16_to_float(&inference.buffers[inference.buf_select ^ 1][offset], out_ptr, length);
    return 0;
}

static void microphone_inference_end(void)
{
    PDM.end();
    free(inference.buffers[0]);
    free(inference.buffers[1]);
    free(sampleBuffer);
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_MICROPHONE
#error "Invalid model for current sensor."
#endif