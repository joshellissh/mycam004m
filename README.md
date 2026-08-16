# mycam004m

Out-of-tree Linux V4L2 subdevice driver for the **MYIR MY-CAM004M**
decoder board: four AHD camera inputs, multiplexed onto a single
4-lane MIPI CSI-2 output as four virtual channels (one per input).
Target platform: **BeaglePlay (TI AM625)** via the J17 CSI connector,
with TI's `j721e-csi2rx` / `cdns-csi2rx` drivers owning the D-PHY, CSI-2
bridge, and DMA on the SoC side.

## Status: structurally complete, register-level TODO

The V4L2/Media-Controller plumbing is real and has been build-tested
against the actual target kernel (see "How this was verified" below).
What's **not** implemented is anything that requires MYIR's
register-level programming documentation for MY-CAM004M / its "N4"
decoder, which wasn't available while writing this. Every such place is
marked `TODO` in the source, and isolated as tightly as possible:

| Unknown | Where it lives | What to do once you have the datasheet |
|---|---|---|
| Init register sequence | `mycam004m_init_regs[]` in `mycam004m-regs.h` | Fill in the array. `mycam004m.c` doesn't need to change. |
| CSI-2 output config (lanes, format, VC assignment) | `mycam004m_csi_output_regs[]` in `mycam004m-regs.h` | Same as above -- but see the two things in `mycam004m.c` that must stay in sync with it, called out in the big comment at the top of `mycam004m-regs.h`. |
| Per-AHD-input enable/disable | `mycam004m_enable_camera_input()` / `_disable_camera_input()` in `mycam004m.c` | Not tabular yet (shape unknown) -- implement directly. |
| CSI-2 TX start/stop | `mycam004m_start_csi_tx()` / `_stop_csi_tx()` in `mycam004m.c` | Likely a single register/bit -- implement directly. |
| Virtual-channel assignment (AHD input N -> VC N) | `mycam004m_vc_for_cam()` in `mycam004m.c` | Confirm or correct the mapping. |
| I2C address (0x30-0x33, SA0/SA1-strap-selected) | `reg = <0x30>` in `dts/k3-am625-beagleplay-mycam004m.dtso` | Confirm with `i2cdetect` against the populated board; DT-only fix. |
| GPIO polarity (RESET_N/PWREN active-low/high) | Same overlay file | Confirm against the MY-CAM004M schematic; DT-only fix. |
| CSI-2 lane bit rate | `link-frequencies` in the same overlay | Currently a placeholder that only satisfies the throughput arithmetic, not a real datasheet value; DT-only fix, no driver code depends on the specific number. |
| Chip-ID readback during probe | Not present | Add once you know the ID register's address/expected value. |

Grep the repo for `TODO` to find all of these plus a few smaller ones
(register width assumption, register-write inter-delay, etc.) with full
context.

## Why the two GPIOs, not three

The task this was written against calls out three MY-CAM004M control
lines: RESET_N, PWREN, PWRDN. BeaglePlay's J17 connector only routes
**two** general-purpose GPIOs to the camera header (`main_gpio0` pins
11/12, labeled `CSI2_CAMERA_GPIO1`/`GPIO2` in TI's own
`k3-am625-beagleplay.dts`) -- confirmed by reading that file directly,
not assumed. Per project decision, RESET_N and PWREN use those two;
PWRDN is left unwired in the overlay. `mycam004m.c` still requests all
three as optional GPIOs (`devm_gpiod_get_optional`), so a future
carrier board that does wire a third GPIO needs no driver change --
just add a `pwrdn-gpios` property to its overlay.

## Repo layout

- `mycam004m.c` -- the driver.
- `mycam004m.h` -- shared constants/structs (pad indices, fixed target
  format, the `struct mycam004m_reg` register-table entry type).
- `mycam004m-regs.h` -- the register tables. **This is the file to
  edit** once MYIR's programming guide is available; see its top
  comment.
- `Kconfig`, `Makefile` -- see "Building" below.
- `dts/k3-am625-beagleplay-mycam004m.dtso` -- devicetree overlay:
  I2C address, reset/pwren GPIOs, 4 CSI-2 lanes, endpoint link to
  `cdns_csi2rx0`.
- `docs/testing.md` -- `media-ctl`/`v4l2-ctl` commands to verify the
  media graph, format negotiation, and (once the register tables are
  filled in) actual 4-stream capture.
- `mycam004m-fake.c` -- a second, independent module: a fake capture
  driver that serves 4 static reference images as real `/dev/video*`
  frame buffers, so a calling app's V4L2 code can be tested end-to-end
  without MY-CAM004M hardware or the register tables. Not part of the
  real driver -- see `docs/fake-driver-testing.md`.
- `tools/gen_fake_frames.py` -- generates the 4 reference images
  `mycam004m-fake.c` serves (`firmware/mycam004m-fake/cam{1..4}.bin`).
- `scripts/select-camera-backend.sh` -- symlinks `/dev/mycam/cam1..4`
  to whichever backend (real or fake) is currently loaded, so the
  calling app's device paths never change between the two.
- `docs/ultima-app-integration.md` -- implementation notes for wiring
  `ultima-app` (in the separate `UltimaGC` repo) up to `/dev/mycam/cam1..4`,
  grounded in its actual existing `camerafeed.cpp`/`cameraview.cpp`.

## Building

This is a standard Kbuild external module. It needs a **configured
and built** kernel tree for the target (`Module.symvers`, `.config`,
generated headers all present) -- not just kernel source, and not the
host's own kernel.

If you're working from this project's BeaglePlay Yocto build
(`~/code/UltimaGC/beagleplay-falcon`), that tree already exists inside
the `falcon-yocto-build` Docker volume at:

```
tisdk/build/arago-tmp-default-glibc/work/beagleplay_ti-oe-linux/linux-ti-staging/6.12.57+git/build
```

and a matching aarch64 cross-toolchain (GCC 13.4.0) exists in the same
volume under `sysroots-components/aarch64/{gcc-cross-aarch64,
binutils-cross-aarch64}`. This is exactly what was used to build-test
this driver -- see "How this was verified" below for the full command;
the short version, run from a container with that volume mounted at
`/home/builder/yocto`:

```sh
KDIR=/home/builder/yocto/tisdk/build/arago-tmp-default-glibc/work/beagleplay_ti-oe-linux/linux-ti-staging/6.12.57+git/build
TOOLBIN=/home/builder/yocto/tisdk/build/arago-tmp-default-glibc/sysroots-components/aarch64/gcc-cross-aarch64/usr/bin/aarch64-oe-linux
BINUTILS=/home/builder/yocto/tisdk/build/arago-tmp-default-glibc/sysroots-components/aarch64/binutils-cross-aarch64/usr/bin/aarch64-oe-linux
export PATH="$TOOLBIN:$BINUTILS:$PATH"

make -C "$KDIR" M=$(pwd) ARCH=arm64 CROSS_COMPILE=aarch64-oe-linux- modules
```

Outside that setup, the same `Makefile` works against any prepared
`KDIR` for your target:

```sh
make KDIR=/path/to/configured/kernel/build ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu- modules
```

This produces `mycam004m.ko`. Copy it to the board and:

```sh
insmod mycam004m.ko
# or, once installed under /lib/modules/$(uname -r)/:
modprobe mycam004m
```

`Kconfig` is **not** wired into any menu by default (this is meant to
be built standalone, per the task). It's provided so the driver can
also be dropped into a real kernel tree (e.g.
`drivers/media/i2c/mycam004m/`) and built in-tree if you'd rather do
that later -- `source` it from the parent `Kconfig` in that case, and
the `Makefile`'s `obj-$(CONFIG_VIDEO_MYCAM004M)` line will pick it up
correctly (the standalone/external-module path forces `obj-m`
regardless, since `CONFIG_VIDEO_MYCAM004M` won't exist in an unmodified
kernel's `.config` -- see the comment in `Makefile`, this was an actual
bug caught by the build-test below and is worth understanding if you
change this file).

### Applying the devicetree overlay

Compile with `dtc` (the kernel tree above already has one under
`scripts/dtc/`) against the same kernel's DT includes, e.g.:

```sh
dtc -@ -I dts -O dtb \
    -o k3-am625-beagleplay-mycam004m.dtbo \
    -i $KDIR/arch/arm64/boot/dts/ti \
    dts/k3-am625-beagleplay-mycam004m.dtso
```

then apply it however your bootloader/overlay mechanism expects
(U-Boot `fdt apply`, `/boot/overlays`, etc. -- specific to how this
project's BeaglePlay build is set up; see
`~/code/UltimaGC/beagleplay-falcon/NOTES.md` for that side of things,
which this repo intentionally doesn't touch).

### Verifying

See `docs/testing.md` for `media-ctl`/`v4l2-ctl` commands to check the
media graph, format negotiation, and streams -- including what's
expected to work already vs. what needs the register tables filled in
first.

### Testing the calling app without hardware or a register table

See `docs/fake-driver-testing.md`. `mycam004m-fake.ko` (built from
this same Makefile) serves 4 static reference images as real
`/dev/video*` frame buffers, so a calling app's open/negotiate/stream
code can be exercised against genuine (if static) delivered pixel
data today, independent of the driver's own TODOs.

## How this was verified

No MY-CAM004M hardware or MYIR documentation was available while
writing this, so "verified" here means something specific and limited:

- **Every non-TODO API call in `mycam004m.c` was checked against real
  usage in TI's own kernel tree** (`ds90ub960.c`, the FPD-Link
  multiplexed-stream decoder driver already in this SDK, plus
  `j721e-csi2rx.c` / `cdns-csi2rx.c`, the actual downstream consumers
  of this driver's `.get_frame_desc`/`.enable_streams` calls), not
  written from memory or guessed. In particular: the requirement to
  expose a `V4L2_CID_LINK_FREQ` control (`cdns-csi2rx.c` calls
  `v4l2_get_link_freq()` against it to configure the D-PHY) and the
  fact that the `.bus.csi2.vc` value returned from `get_frame_desc` is
  what `j721e-csi2rx.c` programs into its hardware VC filter (i.e. it's
  load-bearing, not descriptive) were both confirmed by reading those
  drivers directly, not assumed.
- **The driver actually compiles and links cleanly** (`W=1`, zero
  warnings) into a loadable `mycam004m.ko` against BeaglePlay's real,
  configured `linux-ti-staging` 6.12.57 kernel build tree and the
  matching aarch64 cross-toolchain, both found inside this project's
  own `falcon-yocto-build` Docker volume. `modinfo` on the resulting
  `.ko` confirms correct `depends` (`videodev`, `v4l2-fwnode`, `mc`,
  `v4l2-async`), correct OF/I2C module aliases, and a `vermagic`
  matching that exact kernel build.
- **What this does *not* verify**: actual behavior against real
  MY-CAM004M hardware, correctness of anything still marked TODO, or
  that the devicetree overlay's placeholder values (I2C address, GPIO
  polarity, lane rate) match your populated board.
