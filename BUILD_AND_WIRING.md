# Build and wiring notes

## Prototype pin assignment

All GPIO assignments below are the current firmware defaults for an ESP32-S3 development board.

| Module | Module pin | MCU pin |
|---|---|---:|
| ST7789 TFT | SCL / SCK | GPIO 12 |
| ST7789 TFT | SDA / MOSI | GPIO 11 |
| ST7789 TFT | RES / RST | GPIO 8 |
| ST7789 TFT | DC | GPIO 9 |
| ST7789 TFT | CS | GPIO 10 |
| ST7789 TFT | BL / LED | GPIO 7 |
| Joystick | VRx | GPIO 15 (ADC) |
| Joystick | VRy | GPIO 6 (ADC) |
| Joystick | SW | GPIO 17 (active-low, internal pull-up) |
| Passive piezo | Positive | GPIO 39 (PWM/LEDC) |
| All modules | GND | Common GND |

## Electrical cautions

- Confirm the exact ESP32-S3 board pinout. Board labels and exposed GPIOs vary.
- GPIO 39 is selected for the passive buzzer because the target is ESP32-S3. On the original ESP32, GPIO 39 is input-only and cannot drive the buzzer.
- Use a passive piezo transducer for variable-pitch tones. An active buzzer generally cannot reproduce melodies or throttle-dependent pitch properly.
- If the buzzer draws more current than a GPIO can safely supply, use a transistor driver and an appropriate resistor. Do not drive a high-current sounder directly from the MCU pin.
- Check the TFT breakout's required supply voltage and backlight current. Do not assume every 2.8-inch module is wired identically.
- Join grounds between the ESP32, display, joystick and buzzer.

## Controls and tuning

The joystick centre is currently assumed to be approximately 2048 on a 12-bit ADC, with a deadzone of 250. Actual readings depend on the joystick and ADC configuration. If the craft drifts or the throttle direction feels inverted, inspect `readJoystickInputs()` and adjust the centre, deadzone, axis direction, or mapping.

Gameplay constants such as `MAX_ACC`, `BURN_RATE`, `VY_MAX`, `VX_MAX`, and `ANG_MAX` are defined near the beginning of the sketch.

## Bring-up checklist

1. Power the ESP32-S3 from a stable USB supply.
2. Verify TFT wiring and backlight before connecting the joystick.
3. Upload the sketch and check Serial Monitor at 115200 baud for startup warnings.
4. Confirm title screen and display colours.
5. Check both joystick axes and the push switch.
6. Test buzzer tones at low volume; use a driver stage if required.
7. Test a short flight before attempting a high-multiplier landing.
