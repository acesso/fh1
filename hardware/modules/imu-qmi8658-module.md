---
component: QMI8658 6-axis IMU module (accelerometer + gyroscope), single 8-pin row (GY-521-style pinout), sold as "compatible with BMI160, instead of MPU6050"
image: imu-qmi8658-module.png (seller's photo). The BMI160 alternative: imu-bmi160-dimensions.png, imu-bmi160-pinout.png
role: motion sensor (steps, shake, tilt) for the pet and games
status: preferred by the user 2026-10-31 for its single pin row (mounts upright); chip variant to confirm
---

# QMI8658 IMU module (single row)

## Read from the image
- **Header:** one row of 8 pins: **VCC, GND, SCL, SDA, XDA, XCL, AD0, INT**. This is the MPU6050 / GY-521 layout.
- **Parts:**
  - A small regulator (SOT-23-5, marked `LB2K`).
  - **4.7 kΩ (`472`) pull-ups** on SCL/SDA and XDA/XCL.
  - A few capacitors.
  - **A power LED** with its resistor near INT.
- **Mounting:** two mounting holes, on the side away from the pins.
- **Size:** about 21 × 16 mm, judged from the pin pitch.

## Compared with the BMI160 module (the other listing)
- **BMI160 module:** 18 × 13 mm, **pins on two sides**. One side has VIN, 3V3, GND, SCL, SDA, CS, SA0; the other has OCS, INT2, INT1, SCX, SDX. It also has 3.9 kΩ pull-ups and a regulator. It has to lie flat.
- **QMI8658 module:** **one row**, so it can stand upright at 90° to the board and take one row of holes. This is the reason to prefer it.

## Inferred — needs your confirmation
1. **Chip variant: the deciding point.**
   - QST's **QMI8658A** datasheet lists a built-in **pedometer** (plus tap, any-, no- and significant-motion), and wake-on-motion.
   - Many cheap modules carry the older **QMI8658C**. That one is not confirmed to have the pedometer.
   - With the C, the pet would count steps in firmware, with more S3 wake-ups. The BMI160's hardware step counter would then be the better pick.
   - Check the listing or the chip marking for **"8658A"**.
2. **Current:** QST's datasheet has a current table (section 3.8) that I couldn't read here. Expect tens of µA for accelerometer-only low power. Check before relying on it.
3. **Power LED:** remove it, or cut its resistor. It draws about 1–2 mA all the time.
4. **Pull-ups:** its 4.7 kΩ on SCL/SDA add to the other modules' pull-ups. Count them all on the bus and remove extras, aiming for about 2.2–4.7 kΩ total.

## Use in this project
- **I2C:** at **0x6A** (AD0 low) or **0x6B** (AD0 high). No clash with the codec (0x18), the expanders (0x20/0x21) or the RTC (0x51).
- **XDA/XCL** (the auxiliary master bus) are unused.
- **INT** goes to MCP #2 GPB2 (steps, motion), through board-to-board row 16, and wakes the S3 through the shared expander interrupt.
- **Mounting:** upright on the **second board's inner face**, below the RTC. It moved off the main board, where its row blocked the button wires. It shares the RTC's I2C, 3V3 and GND from the board-to-board header. It stands well inside the 25 mm gap. Fix it with a dab of glue, because the IMU reads the board's motion, so it shouldn't wobble on its pins.
