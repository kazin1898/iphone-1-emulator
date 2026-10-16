# iPhone OS 3.1.3 (7E18) on iPhone1,1 (M68)

Status: 2026-10-15. The native tool boots the original iPhone1,1 7E18 kernel
on the M68 board and reaches the lock screen. Touch input does not unlock it yet.

## What works

- `tools/iphone1 --board m68 --ios3` drives `s5l_bringup()`, the iOS 3
  front end imported from S5LBox, which nothing called before.
- The kernel gate in `tools/ios3/` comes from S5LBox (MIT). It accepts the
  M68 kernel as a second exact identity: SHA-256 `f36a88d6…6ce9`. The UUID,
  segments, entry point and every patch-site byte match the iPhone1,2 build.
  Only the five base sites are patched: the RTC wait, the BSD root, the
  memory-disk read and write, and the raw watcher. The three pppserial packet
  sites stay stock until a packet bridge exists.
- Bring-up now takes the Merlot node from the board: `arm-io/spi1/lcd0` on
  M68, behind the spi0 MPL bridge. AppleMerlotLCD accepts the N82 panel ID
  `0x00a5c22b`.
- M68 un-matches `arm-io/mbx`, `arm-io/sha1`, `arm-io/uart1/baseband` and
  `arm-io/usb-otg`. It keeps spi2, because the Z1 touch controller is there;
  on N82 spi2 is the baseband bus.
- Measured run to the lock screen: launchd at about 0.29 G instructions, the
  WM8758, PL080 and multitouch drivers start, the Z1 firmware download (44,484
  bytes) completes, and SpringBoard draws the lock screen by about 3.0 G
  instructions. That is about 100 s natively at about 30 MIPS.

## Preparing the root volume

The stock 7E18 HFSX volume has `freeBlocks == 0`, and its fstab names NAND
partitions this machine does not have. Each step below comes from a measured
failure:

| Change | Failure without it |
| --- | --- |
| `fstab`: `/dev/md0 / hfs rw,update 0 0` | `fsck` cannot find `/dev/disk0s1` (or dies with SIGFPE on `rmd0`), and launchd halts |
| `hfsx_grow.py IMAGE 96` | nothing can be allocated on `/`, so boot stalls behind the Apple logo and its progress bar |
| SpringBoard `CA_ENABLE_MBX2D=0` | MBX is un-matched, so QuartzCore's MBX2D context is NULL; SpringBoard dies at 0x48 and respawns forever |
| `data_ark.plist` `FactoryActivated` | lockdownd reports `Unactivated` and enables the brick state |

```sh
cp rootfs_7E18_hfs.img grown.img
python3 tools/firmware/hfsx_grow.py grown.img 96
python3 tools/firmware/rootfs_work.py grown.img work_7E18.img \
    --fstab "/dev/md0 / hfs rw,update 0 0" \
    --launchd-env com.apple.SpringBoard.plist=CA_ENABLE_MBX2D=0 \
    --add /private/var/root/Library/Lockdown/data_ark.plist=tools/firmware/data_ark_activated_3x.plist
./build/tools/iphone1 --board m68 --ios3 --kernel kernel_7E18.macho \
    --devicetree devicetree_m68ap.bin --rootfs work_7E18.img \
    --serial run.log --screenshot run.ppm --steps 4000000000
```

`rootfs_work.py` used to create entries only from catalog templates with
1A543a IDs. When those IDs are absent, it now uses `/private/var/root` and
`/private/etc/fstab` as templates. `--rootfs` is written in place, so always
boot a copy.

The kernel and device tree come from the IMG3 files with their published
keys. `img3.c` now decrypts the final partial AES block; without that, the
kernelcache was 42 bytes short.

## Next gates

1. **Touch.** The 3.1.3 AppleMultitouchSPI reads only the 8-byte header of a
   Z1 frame (op 0x64) and never the payload. After six reads it power-cycles
   the controller and downloads the firmware again. The Z1 model encodes the
   OS 1.0 frame layout, so the 3.x read protocol has to be traced from the
   driver.
2. **Browser.** `web/emu_api.c` still calls only `s5l_os1_boot()`.
3. **Cydia and networking.** S5LBox has a packet bridge (PPP over uart4) and
   an APT installer path, but neither is ported.
