# Padfix Android

Android application project for Padfix gamepad calibration and input remapping.

## Current source baseline

- Padfix V10 native C backend: analog correction for ABS_X/ABS_Y/ABS_RX/ABS_RY and ABS_HAT0X/ABS_HAT0Y to BTN_DPAD_* conversion.
- Dashboard HTML baseline: local web dashboard for calibration, range, offset, button remapping, and status monitoring.
- V11 design notes: dashboard presets and Android joystick-event interception are documented in the source notes.

## Android app work

The Android wrapper (Kotlin/WebView), Shizuku integration, native ARM64 build, and CI APK packaging are not yet complete. Device permissions for `/dev/input` and `/dev/uinput` must be verified on the target phone; a successful build alone does not prove runtime compatibility.

## Build status

Not yet built or tested as an APK.
