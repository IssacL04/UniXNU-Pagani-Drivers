# PaganiInput

Physical input for the OnePlus 13T PKX110, using the original 21H461 `IOHIDDevice` parser and event dispatch. `pagani-buttons=1` starts the PMIC keys; `pagani-touch=1` starts a separate S3910 digitizer. Stage7Probe starts them after the original HID resources and the GOP display are available. Their failures are reported without failing display startup.

The S3910 path adopts QUP1 S4, configures the board pins, resets the controller, disables HBP raw reports and selects the vendor's on-controller LBP mode. It installs and reads back a bounded TouchComm v1 report format before accepting contacts. Ten stable contact IDs map to 0..32767 coordinates; finger and glove classifications are accepted, palms are dropped, and removed contacts receive explicit release reports. A transport failure releases held contacts and disables that device until reboot. No controller firmware is uploaded or erased.

The PMIC observer reads PON SID 0 peripheral 0x13 and GPIO SID 1 peripheral 0x8d. On this board PON bit 7 is Power, PON bit 6 is Volume Up, and the active-low GPIO is Volume Down. A 32 ms filter debounces both edges; failed reads release all keys. The driver never writes PMIC peripheral configuration.

Board addresses and wiring are supplied by the device's `PaganiPeripherals.h`. SM8750 clock and SPMI operations come from `Sm8750Platform/PeripheralIO.h`. SPI uses the selected `QcomGeniSpi` component. The main `build-pagani-peripherals.py` qualifies the original 207-slot HID ABI and the compiled subclass; `build-stage7-display.py` includes the late-registered objects in its archive.

Run `python3 <device>/scripts/test-peripherals.py --tree <resolved-tree>` for the portable transport, contact, debounce and MMIO tests. The tests use simulated hardware. The HID descriptor layout follows [NativeHidInput](https://github.com/UniXNU/NativeHidInput/tree/5f3a37c), with its notice retained in `LICENSE.HID`.

Current limits: ATTN is sampled every 8 ms and keys every 16 ms; GIC interrupts and system-sleep wake are not implemented. SPI uses FIFO PIO and retained GENI microcode. Cold regulator enable, GPI DMA, TouchComm v2, oversized command fragmentation and firmware recovery are not implemented. The firmware must expose LBP and an application identification report. Real contact orientation, report continuity and system edge gestures still need device calibration. Host tests do not establish usable touchscreen input.
