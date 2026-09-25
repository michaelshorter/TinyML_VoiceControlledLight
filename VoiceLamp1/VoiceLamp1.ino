#define EIDSP_QUANTIZE_FILTERBANK   0
#define EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW 4

/* Includes ---------------------------------------------------------------- */
#include <PDM.h>
#include <VoiceLamp_inferencing.h>
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

// ==== LAMP: NeoMatrix setup ====
#define LED_PIN 6
Adafruit_NeoMatrix matrix(8, 8, LED_PIN,
  NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_ROWS + NEO_MATRIX_ZIGZAG,
  NEO_GRB + NEO_KHZ800);

const float CENTER_X = 3.5, CENTER_Y = 3.5;

// Shape = size/brightness (what "light"/"dim"/"off" control)
// Colour = hue only (what "red" controls) - kept separate so one can
// change without resetting the other
struct ShapeTarget { float radius; uint8_t brightness; };
struct ColorTarget { uint8_t r, g, b; };

ShapeTarget SHAPE_LIGHT = {4.5, 60};
ShapeTarget SHAPE_DIM   = {3, 25};
ShapeTarget SHAPE_OFF   = {0.0, 0};

ColorTarget COLOR_WARM = {255, 200, 120};
ColorTarget COLOR_RED  = {255, 0,   0};

const float THRESHOLD_LIGHT = 0.85;
const float THRESHOLD_DIM   = 0.80;
const float THRESHOLD_OFF   = 0.80;
const float THRESHOLD_RED   = 0.85;

ShapeTarget activeShape = SHAPE_OFF;
ColorTarget activeColor = COLOR_WARM;

float currentRadius = 0;
float currentBrightness = 0;
float currentR = 0, currentG = 0, currentB = 0;

unsigned long lastCommandTime = 0;
const unsigned long COOLDOWN_MS = 1500;
const float EASE_FACTOR = 0.10;  // lowered - animation now steps far more often

unsigned long lastFrameTime = 0;
const unsigned long FRAME_INTERVAL_MS = 20;  // ~50fps

void setLampCommand(const char* label, float confidence) {
  if (millis() - lastCommandTime < COOLDOWN_MS) return;

  if (strcmp(label, "light") == 0 && confidence >= THRESHOLD_LIGHT) {
    activeShape = SHAPE_LIGHT;
    activeColor = COLOR_WARM;
  } else if (strcmp(label, "dim") == 0 && confidence >= THRESHOLD_DIM) {
    activeShape = SHAPE_DIM;
    activeColor = COLOR_WARM;
  } else if (strcmp(label, "off") == 0 && confidence >= THRESHOLD_OFF) {
    activeShape = SHAPE_OFF;
    // colour left as-is deliberately - doesn't matter at brightness 0
  } else if (strcmp(label, "red") == 0 && confidence >= THRESHOLD_RED) {
    activeColor = COLOR_RED;
    // activeShape deliberately untouched - keeps current size/brightness
  } else {
    return; // background/unknown, or didn't meet its own threshold
  }

  lastCommandTime = millis();
}

void updateLampAnimation() {
  currentRadius += (activeShape.radius - currentRadius) * EASE_FACTOR;
  currentBrightness += (activeShape.brightness - currentBrightness) * EASE_FACTOR;
  currentR += (activeColor.r - currentR) * EASE_FACTOR;
  currentG += (activeColor.g - currentG) * EASE_FACTOR;
  currentB += (activeColor.b - currentB) * EASE_FACTOR;

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
          (uint8_t)(currentR * intensity), (uint8_t)(currentG * intensity), (uint8_t)(currentB * intensity)));
      }
    }
  }
  matrix.show();
}
// ==== END LAMP ====

void setup()
{
    Serial.begin(115200);
    //while (!Serial);
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

    // ==== LAMP: find best-scoring class and pass it to the command handler ====
    size_t best_ix = 0;
    for (size_t ix = 1; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        if (result.classification[ix].value > result.classification[best_ix].value) {
            best_ix = ix;
        }
    }
    setLampCommand(result.classification[best_ix].label, result.classification[best_ix].value);

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

    // ==== LAMP: step the animation every loop, regardless of print throttle ====
    updateLampAnimation();
}

// ---- everything below here is UNCHANGED from your original exported file, except microphone_inference_record() ----

static void pdm_data_ready_inference_callback(void)
{
    int bytesAvailable = PDM.available();
    int bytesRead = PDM.read((char *)&sampleBuffer[0], bytesAvailable);

    if (record_ready == true) {
        for (int i = 0; i<bytesRead>> 1; i++) {
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
        // Use this wait time to keep the animation smooth, rather than
        // sitting idle until the next audio slice is ready
        if (millis() - lastFrameTime >= FRAME_INTERVAL_MS) {
            updateLampAnimation();
            lastFrameTime = millis();
        }
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