---
name: testing-web-emulator
description: How to run and UI-test the browser (WebAssembly) build of the iPhone 1 emulator in web/ — serving it, loading firmware, clicking guest UI, and timing expectations.
---

# Testing the browser build of the iPhone 1 emulator

## Serve it
- Prebuilt artifacts live in `web/dist/emu.js` + `web/dist/emu.wasm`; firmware/snapshot assets in
  `web/firmware` (~178 MB on disk, gzip `.part0/.part1` files). Neither is fetched from the network.
- Run `python3 -m http.server 8080` from `/path/to/repo/web` and open `http://127.0.0.1:8080/`.
- Rebuild only if `web/dist` is missing:
  `source ~/emsdk/emsdk_env.sh && emcmake cmake -S . -B web/build -DIPHONE1_WEB=ON -DCMAKE_BUILD_TYPE=Release && cmake --build web/build`
- `web/manifest.json` decides cold boot vs snapshot. When a `snapshot` entry is present the guest
  resumes directly at the iPhone OS home screen (seconds); without it, cold boot from `rootfs`
  takes many minutes. Prefer the snapshot path for UI tests.

## Layout / interaction facts (web/index.html, web/app.js)
- Canvas is 320x480 backing store, displayed at 640x960 CSS on viewports ≥700px, so guest
  coordinates are CSS pixels / 2. `point()` uses `getBoundingClientRect`, so scrolling the page is safe.
- The status line under the canvas reads
  `Running · <N> instructions/s · cycle <N> · scanout 320×480`. On a 1600x1200 display, maximize the
  window and scroll down ~3 clicks so both the canvas and the status line are visible at once.
- Keyboard: Escape or H = Home, P = Hold, +/- = volume. These are captured with `preventDefault`,
  so **Ctrl+- will not zoom the browser** — it reaches the guest as volume-down (a ringer HUD appears).
  Use the zoom menu or resizing instead. Click the page background before sending Escape so the
  document has focus.
- Guest icon coordinates (320x480 guest space): Settings ≈ (266, 255); dock ≈ Phone (40, 443),
  Mail (120, 443), Safari (200, 443), iPod (280, 443).

## Timing and flakiness
- The emulator runs ~15x slower than real time (~26–30M instructions/s on a dev box). Allow
  20–40 s wall clock for each UI reaction; an app-switch shows a black screen for 10–20 s first.
- An instantaneous synthetic click (pointerdown+pointerup in the same millisecond) can be missed by
  the guest's touch stack. If a tap appears to do nothing, retry with a held press:
  `mouse_move` → `left_mouse_down` → wait ~1 s → `left_mouse_up`. This made the Safari dock icon
  launch reliably after two instant clicks were ignored.
- After a first load the assets are HTTP-cached, so a reload goes through
  "Downloading …" → "Initializing the M68 machine…" → "Loading snapshot…" → Running in ~20 s and the
  download-progress text may be too brief to screenshot. Use a hard reload with the network
  throttled/cache disabled if you must capture the progress text.

## Devin Secrets Needed
None — everything is local and static.
