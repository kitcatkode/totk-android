# The Android app

`build_android.py --apk` also builds `totk.apk`. It holds the same runtime as `totk-native`, built as `libtotk.so` and
started by Android's NativeActivity (`runtime/frontend_android.c`), so the game runs inside the app as it does on iOS.
The APK has no Java of its own. `android/apk/build_apk.py` writes the manifest and signs the APK without the Android
SDK.

## Install

```
python3 android/build_android.py <game.nsp> -k <prod.keys> --firmware <firmware dir> --ndk <NDK> --runtime-only --apk
sh <TOTK_android>/install_app.sh
sh <TOTK_android>/install_app.sh from-device
```

1. The build writes `totk.apk` and `install_app.sh` into the output folder (`TOTK_android`).
2. `install_app.sh` installs or updates the app.
3. `from-device` fills the game folder once. It copies `rewritten/`, `extracted/`, `shaders/`, `user-data/` (the saves),
   `driver.so` and `totk.env` from the adb-shell folder `/data/local/tmp/totk` into the app's folder on the device.
   That's about 17 GB, so it takes a few minutes. `install_app.sh data` copies from the computer instead.

The first build makes a signing key, `apk-key.pem`, in the work folder (`TOTK_build`). Later builds must be signed with
the same key, or Android refuses the update. Uninstalling the app deletes its folder, saves included.

## The game folder

`/sdcard/Android/data/dev.kitcatkode.totk/files` (shown as `Android/data/dev.kitcatkode.totk/files` in file managers)
holds what the adb-shell folder holds:

| | |
| --- | --- |
| `rewritten/<version>/` | the game's code (one version folder) |
| `extracted/` | RomFS, shared fonts |
| `shaders/<version>/` | the Vulkan shader seed |
| `driver.so` | the Vulkan driver (a Turnip build). It's copied into the app's private folder at each start, because files on shared storage cannot be loaded as code. Without it, the system driver is used. |
| `user-data/` | saves to import: on the first start (no private saves yet) they're copied into the app's private storage, where the game keeps its saves from then on. `install_app.sh saves` copies those to the computer. |
| `totk.env` | options, one `NAME=value` per line (`#` comments), e.g. `TOTK_VK_SHOT=300` or `TOTK_FACE_BUTTONS=position` |
| `totk.log` | the log of the last start (`install_app.sh log`) |

## Controls

Face buttons are mapped by name: the Thor's A is the Switch's A, B is B, and so on (the Thor reports its buttons by
their Nintendo-style labels). For an Xbox-layout gamepad, `TOTK_FACE_BUTTONS=position` maps them by position instead
(the bottom button is the Switch's B, the right one A, the left one Y, the top one X).

| Thor / Android | Switch |
| --- | --- |
| L1 / R1 | L / R |
| L2 / R2 (button or trigger axis) | ZL / ZR |
| stick clicks | L / R stick |
| Start / Select | + / − |
| d-pad (keys or hat) | d-pad |

The Back key does nothing, so a stray press doesn't leave the game.

## The picture

Each frame the game presents is copied by the GPU into one of three host-visible buffers, and a presenter thread writes
it into the window (`ANativeWindow_lock`). The compositor scales the 1280×720 picture to the screen. This works with any
driver, including a Turnip loaded directly, which brings no swapchain of its own (Android's Vulkan loader provides
those). A zero-copy path (AHardwareBuffer and SurfaceControl) can replace it later.

## The second screen

On a device with a second (presentation) display, such as the AYN Thor's bottom screen, the game's activity opens the
app's own screen there (`runtime/bottom_screen.c`; `TOTK_BOTTOM=0`: off). It is the same `NativeActivity` and library
under a second name (the manifest's activity-alias `<package>.Bottom`, started on that display), drawn in software with
the system's Roboto font (stb_truetype, `third_party/stb`).

- **Settings** tab: tap a row to cycle its value; it is saved to `totk.env` at once (lines the screen does not manage
  are kept). The frame generation rows (on / 2x / 3x, mode, HUD) also apply at once: frame generation stops and starts
  again with them at the next frame, and the window asks for the new refresh rate. A row whose saved value differs
  from what the running game uses says "applies after a restart".
  Rows: frame generation (off / 2x / 3x), its mode (quality / performance), the HUD kept out of it or mixed in, the
  frame cap (30 / 40 / 60), the render resolution (720p / 612p / 540p), the shadow resolution (720 / 1024,
  `TOTK_SHADOW_RES`) and gyro aiming. The line under the tabs shows the game's frames a second.
- **Map**: the three map layers from the game files with Link's live position; pinch, drag, double-tap, "Whole map" and
  "Follow Link". **Inventory**: the pouches from the newest save, read-only. Details: [BOTTOM_SCREEN.md](BOTTOM_SCREEN.md).
- Touch goes to this screen; the controller always drives the game, whichever screen has focus.
