# Starship Lander 🚀

A joystick-controlled lunar-landing arcade game built with an **ESP32-S3** and a **2.8-inch ST7789 TFT display**. Pilot a small spacecraft, manage thrust and fuel, control your descent, and land safely on one of two landing pads.

Mission Control is part of the experience: it comments on your flight, warns you when things get uncomfortable, celebrates successful touchdowns, and has a few words ready when you meet the terrain too enthusiastically.

> **Status:** Hobby/learning project. The firmware is provided for experimentation and has not been validated across every ESP32-S3 board or display module. Check the pin mapping for your exact hardware before powering up.

## Features

- Real-time, joystick-based spacecraft steering and throttle.
- Gravity, thrust, velocity, tilt, and fuel-management gameplay.
- Procedurally generated terrain and two landing pads with different score multipliers.
- Landing checks for vertical speed, horizontal drift, and spacecraft angle.
- Mission Control commentary that reacts to flight conditions.
- Passive-buzzer sound effects for launch, engine, warnings, landing, and crash events.
- Starfield, spacecraft graphics, engine exhaust, and ground-dust particles.
- Score and persistent high score stored using ESP32 Preferences/NVS.
- Increasing difficulty across levels.

## Hardware

| Part | Purpose |
|---|---|
| ESP32-S3 development board | Game logic, physics, display and audio control |
| 2.8-inch 240×320 ST7789 TFT | Game display |
| 2-axis analog joystick with push switch | Steering, throttle and menu/action input |
| Passive piezo buzzer | Audio feedback |
| Suitable power source and jumper wires | Power and interconnections |

## Pin mapping

The pin definitions are at the top of `Starship-Lander.ino` and can be changed to suit your board.

| Signal | ESP32-S3 GPIO |
|---|---:|
| TFT SCK / SCL | 12 |
| TFT MOSI / SDA | 11 |
| TFT RST | 8 |
| TFT DC | 9 |
| TFT CS | 10 |
| TFT backlight | 7 |
| Joystick X / VRx | 15 |
| Joystick Y / VRy | 6 |
| Joystick switch / SW | 17 |
| Passive buzzer + | 39 |
| Common ground | GND |

**Important:** This pinout assumes an ESP32-S3. GPIO 39 is output-capable on the ESP32-S3, but GPIO 39 is input-only on the original ESP32. Do not use this buzzer pin unchanged on an original ESP32. Verify the pin capabilities and board pinout for your exact module.

Connect the TFT's power pin only to the voltage specified by its breakout-board documentation. Some modules accept 5 V on their power input because they include a regulator; others are 3.3 V only. Logic signals should be compatible with the ESP32-S3's 3.3 V GPIO.

## Software requirements

- Arduino IDE 2.x or Arduino CLI.
- Espressif ESP32 board package (Arduino-ESP32 core 2.x or 3.x).
- `Adafruit GFX Library`.
- `Adafruit ST7735 and ST7789 Library`.

The sketch uses the ESP32 `Preferences` library, which is included with the ESP32 Arduino core. The buzzer code contains separate LEDC handling for ESP32 Arduino core 2.x and 3.x.

## Build and upload

1. Install the ESP32 board package in Arduino IDE and select your exact ESP32-S3 board.
2. Install **Adafruit GFX Library** and **Adafruit ST7735 and ST7789 Library** through Library Manager.
3. Open `Starship-Lander.ino`.
4. Confirm the pin mapping against your wiring and board documentation.
5. Connect the board, select its port, compile, and upload.
6. Open Serial Monitor at **115200 baud** if you need startup diagnostics.

If the display is blank or colours are incorrect, check the display controller, wiring, rotation, backlight polarity, and `INVERT_COLORS` setting. If the joystick direction is reversed, adjust the axis mapping in `readJoystickInputs()`.

## How to play

- Press the joystick to launch from the title screen.
- Move the joystick horizontally to tilt the spacecraft.
- Move the throttle axis to control engine thrust (the active direction depends on the joystick module's orientation).
- Keep fuel in mind: thrust consumes it.
- Land on one of the highlighted pads with low vertical speed, low horizontal drift, and a near-upright angle.
- The two pads have different score multipliers. The narrower/higher-value pad is harder to reach.
- Press the joystick after a successful landing to proceed to the next level. After a crash, press it to return to the title screen.

The current tuning constants (acceleration, gravity, burn rate and landing limits) are defined near the top of the sketch.

## Repository structure

```text
Starship-Lander/
├── Starship-Lander.ino
├── README.md
├── LICENSE
├── .gitignore
└── docs/
    ├── BUILD_AND_WIRING.md
    └── LINKEDIN_POST.md
```

## Known limitations / next steps

- The pinout and display settings are tailored to the current prototype; adapt them for other boards and TFT breakouts.
- Joystick centre and deadzone are currently configured with fixed ADC values. Calibration could improve compatibility with different joystick modules and ADC configurations.
- The project has not been tested against every ESP32-S3 variant, TFT breakout, or Arduino-ESP32 release.
- Future improvements could include a calibration screen, pause function, settings menu, improved collision geometry, and recorded best-run statistics.

## Credits

Designed and developed as an embedded-systems and interactive-display experiment. Built with Arduino, ESP32, Adafruit GFX, and the Adafruit ST7789 display library.

## License

Released under the MIT License. See [`LICENSE`](LICENSE).
