# VoiceLamp

A voice-controlled lamp built from an 8x8 NeoPixel matrix and an Arduino Nano 33 BLE Sense. A small TinyML keyword-spotting model, trained in Edge Impulse, listens continuously for four spoken commands and drives a warm, pulsing light animation on the matrix.

This is primarily a personal record of the build. Notes on reproducing the model from scratch are included below, but the point of this repo is to keep the working files and the reasoning behind them in one place.

## Project links

- Edge Impulse project: https://studio.edgeimpulse.com/studio/1118568

## Commands

| Command | Effect |
|---|---|
| light | Matrix fades up to a full warm-white glow |
| dim | Matrix shrinks down to a small centred glow |
| off | Matrix fades to fully off |
| red | Current shape and brightness stay the same, colour switches to red |

Commands transition smoothly rather than snapping, using an eased animation rather than an instant cut.

## Hardware

- Arduino Nano 33 BLE Sense (Rev1 or Rev2, uses the onboard PDM microphone)
- 8x8 NeoPixel/WS2812 matrix (64 pixels)
- 5V power supply capable of a few hundred mA (USB power bank, wall adapter, or the Nano's own USB connection once the 5V pin jumper is bridged)
- A 300-500 ohm resistor in series on the matrix data line
- Optional but recommended: a 74AHCT125 (or similar) logic level shifter between the Nano's data pin and the matrix, since the Nano runs 3.3V logic and WS2812s expect closer to 5V

## Wiring

- Matrix data in -> Nano D6, through the series resistor
- Matrix VCC -> Nano 5V pin (requires bridging the solder jumper on the underside of the board, since this pin is disabled by default) or an independent 5V supply
- Matrix GND -> Nano GND (common ground is essential)
- Do not power the matrix from the Nano's Vin or 3.3V pins. Vin is meant as an input, and 3.3V sits under the WS2812's minimum spec.

## Libraries required

Install through the Arduino IDE Library Manager unless noted otherwise:

- Adafruit NeoPixel
- Adafruit GFX Library
- Adafruit NeoMatrix
- Adafruit BusIO (dependency of the above)
- Arduino Mbed OS Nano Boards (board package, installed via Boards Manager rather than Library Manager)
- The Edge Impulse inferencing library exported from the project linked above. The exported zip is included in this repo under `lib/VoiceLamp_inferencing.zip` so the whole working setup is kept together, rather than needing to regenerate it from Edge Impulse to get back to a working build.

## Building the model in Edge Impulse

This repository contains the firmware only. To reproduce the model:

1. Create an Edge Impulse project and connect the Nano via the Edge Impulse CLI daemon (`edge-impulse-daemon`), which requires the Nano to be running Edge Impulse's own data-forwarding firmware rather than this sketch.
2. Record samples for six classes: `light`, `dim`, `off`, `red`, `background` (ambient noise), and `unknown` (other speech, near-miss words). Crop each command sample to close to 1000ms, including a natural buffer of real recorded silence either side of the spoken word rather than trimming tightly, and rather than relying on Edge Impulse's automatic zero-padding to fill the gap.
3. Create an impulse with a 1000ms window and 125ms window increase (stride), an MFCC processing block, and a Classification (Keras) learning block.
4. Generate features and train.
5. In the Deployment tab, choose "Arduino library", Quantized (int8), and build. This downloads a zip containing the trained model as an Arduino library.
6. Install that zip in the Arduino IDE (Sketch > Include Library > Add .ZIP Library).

Full background on why the sample length and cropping approach matters is in [Notes](#notes) below.

## Flashing

1. Open `VoiceLamp.ino` in the Arduino IDE.
2. Select Tools > Board > Arduino Mbed OS Nano Boards > Arduino Nano 33 BLE.
3. Confirm the Edge Impulse library is installed and that `#include <VoiceLamp_inferencing.h>` matches your library's actual name (rename the include if your Edge Impulse project has a different name).
4. Upload. If the board doesn't upload immediately, double-tap the physical reset button to drop it into bootloader mode (the LED will slow-pulse) and try again.

## Tuning

A few constants near the top of the sketch are worth adjusting to taste or to match a retrained model:

- `THRESHOLD_LIGHT`, `THRESHOLD_DIM`, `THRESHOLD_OFF`, `THRESHOLD_RED`: per-command confidence thresholds. Set these relative to how well each class actually performs in your model's confusion matrix, not to one fixed number across all four.
- `SHAPE_LIGHT`, `SHAPE_DIM`, `SHAPE_OFF`, `COLOR_WARM`, `COLOR_RED`: the target radius, brightness, and colour for each state.
- `EASE_FACTOR`: how quickly the animation glides towards a new target. Lower is slower and smoother, higher is snappier.
- `COOLDOWN_MS`: minimum time between accepted commands, to stop a single spoken word triggering multiple times as the sliding window catches its tail.

## Notes

A few things learned the hard way while building this, in case they save time on a similar build:

- **Zero-padding versus natural silence.** Edge Impulse pads any training clip shorter than the window with digital silence (true zero amplitude). Live audio from a microphone is never truly silent, it always has some noise floor. A model trained on tightly cropped clips full of digital-zero padding sees a mismatch at deployment time, since the "quiet" parts of a live sliding window aren't digital zero. Cropping command samples to include a natural buffer of real recorded quiet, sized to roughly match the window length, avoids this mismatch while still keeping consistent word boundaries.
- **Stride and memory are linked.** Reducing the window stride increases how many overlapping slices the model needs to process per window, which increases the scratch memory required for the sliding MFCC computation on-device. Pushing stride too low will fail outright with a scratch buffer allocation error rather than degrading gracefully. 125ms stride on a 1000ms window (8 slices) runs reliably on the Nano 33 BLE Sense; more aggressive settings weren't stable in testing.
- **The 5V pin needs a solder jumper.** It's disabled by default on the Nano 33 BLE Sense. Without bridging it, wiring the matrix to that pin does nothing, and it can look like a wiring fault rather than a missing bridge.
- **`while (!Serial)` blocks forever without a computer attached.** The original Edge Impulse example waits indefinitely for a serial connection before continuing, which is fine when a computer's plugged in for debugging but means the board never starts up when powered from a standalone supply. This sketch uses a timeout instead.

## Licence

The Edge Impulse SDK code retained in this sketch is Apache 2.0 licensed by Edge Impulse Inc. The lamp control code (everything marked LAMP) is provided as-is with no licence restrictions, use it however's useful.
