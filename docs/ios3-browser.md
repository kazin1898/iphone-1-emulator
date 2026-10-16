# iPhone OS 3.1.3 in the browser

The browser can run iPhone OS 3.1.3 (7E18) on the original iPhone (M68)
alongside the published 1.0 phone. The 3.1.3 firmware is prepared locally
from your own IPSW. It is Apple software and is never committed or published.

## Prepare the firmware

Requirements: the native build (`build/tools/iphone1`, see the README or
`docs/windows-build.md`), the firmware tool requirements
(`pip install -r tools/firmware/requirements.txt`), and this build's
published firmware keys as JSON. The keys must cover the kernelcache,
`DeviceTree.m68ap.img3` and the root filesystem DMG.

```sh
python3 tools/firmware/prepare_ios3_web.py iPhone1,1_3.1.3_7E18_Restore.ipsw \
    web/firmware/7E18 --keys keys_7E18.json --iphone1 build/tools/iphone1
```

The script decrypts the kernel and device tree, then decrypts, grows and
provisions the root volume. The steps are those in `plans/ios3-m68.md`. It
then boots the volume natively to the lock screen (`--steps`, default
3.6 G instructions, about 2 minutes) and writes a manifest describing the
paired snapshot and disk:

| File | Size |
| --- | --- |
| `kernel_7E18.macho` | 7.9 MB |
| `devicetree_m68ap.bin` | 38 KB |
| `disk_7E18.img` | 534 MB |
| `home_7E18.snap` | 115 MB |
| `manifest.json` | `"kind": "ios3"` |

Without `--iphone1`, there is no snapshot and the page cold boots.
`web/firmware/*/` is ignored by git.

## Use it

Serve `web/` (`npm run serve`) and open the page. The **Firmware** picker
under the phone lists every build in `web/firmwares.json` whose manifest this
server has, and switches with `?firmware=7E18`. When no local firmware
exists, the picker stays hidden. The page without the parameter is
unchanged: the 1.0 phone, its browser saves and offline visits.

Measured locally (Chrome, Windows 11): ready at the lock screen 3.3 s after
navigation, about 25 M instructions/s, 873 MiB WASM heap. Slide to unlock,
taps, app launch (Calculator) and the Home button work. The guest runs on
the interpreter only, so it is slow.

## Current limits

- **No browser saves.** The root volume lives outside guest RAM, behind the
  memory-disk bridge, and checkpoints capture RAM only. Save, Export and
  Import stay off, and the page never takes the 1.0 profile. Every visit
  starts again from the shipped snapshot.
- **No seed JIT.** Its generated regions were compiled from OS 1.0 code and
  are keyed by address, not contents, so 3.1.3 runs interpreted.
- **No offline package** for local firmware, and no network or Wi-Fi in the
  guest.
- The disk is streamed into the WASM heap without Cache Storage or SHA-256
  checks, so it only loads from this site.
