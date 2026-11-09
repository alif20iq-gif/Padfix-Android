# Padfix Android

Android wrapper for the Padfix V10 native input backend and V11 dashboard.

## Included

- `native/padfix.c`: C backend for analog correction and D-pad HAT-to-key conversion.
- `app/src/main/assets/index.html`: V11 dashboard with local preset save/load/delete.
- `app/src/main/java/id/padfix/android/MainActivity.kt`: WebView wrapper, joystick motion interception, and Shizuku shell launcher.
- `.github/workflows/android.yml`: builds an arm64 native executable and debug APK, then uploads the APK as a workflow artifact.

## Build

Pushes to `main` start the GitHub Actions workflow. The build compiles the native backend for arm64, then runs `gradle assembleDebug`. Download the `Padfix-Android-debug` artifact from the completed workflow run.

## Runtime requirements and limitations

- Shizuku must be running and Padfix must be granted Shizuku permission.
- The backend needs working access to `/dev/input` and `/dev/uinput`; this varies by Android build/device and is not guaranteed by Shizuku alone.
- The controller name match in the current C backend defaults to `Pro Controller`.
- This first APK integration pass has not yet been tested on the target phone or with a physical controller. A successful CI build does not prove the kernel input permissions or virtual gamepad work at runtime.
