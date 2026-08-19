# mycam004m

Out-of-tree Linux V4L2 subdevice driver for the **MYIR MY-CAM004M**
decoder board: four AHD camera inputs, multiplexed onto a single
4-lane MIPI CSI-2 output as four virtual channels (one per input).
Target platform: **BeaglePlay (TI AM625)** via the J17 CSI connector,
with TI's `j721e-csi2rx` / `cdns-csi2rx` drivers owning the D-PHY, CSI-2
bridge, and DMA on the SoC side.

## Status: register tables populated, NOT tested against real hardware

The V4L2/Media-Controller plumbing is real and build-tested against the
actual target kernel (see "How this was verified" below). The
register-level unknowns that used to block this are now resolved: MYIR
supplied the actual Nextchip N4 datasheet, MY-CAM004M board schematic,
and product manual (in `MY-CAM004M/`), and researching the remaining
gaps turned up two independent, real, register-compatible sibling-chip
(Nextchip NVP6324) drivers that supply the one thing N4's own
(preliminary, Rev 0.0) datasheet doesn't cover: the MIPI PLL/lane-rate
register recipe.

**None of this has been run against real MY-CAM004M hardware.** Every
value below is either primary-sourced (N4's own datasheet) or
cross-validated (matching almost byte-for-byte between two
independently-written sibling-chip drivers) -- see the citations in
`mycam004m-regs.h` for each one -- but "well-evidenced" isn't "proven."

| Unknown | Status | Source |
|---|---|---|
| Chip identity | confirmed | `MY-CAM004M/Datasheet-N4.pdf` (Nextchip N4, Rev 0.0, 2017) |
| I2C address (0x30) | confirmed | Board schematic labels it directly ("I2C add 0x61"); matches N4's SA0/SA1 address formula |
| GPIO polarity (RESET_N active-low, PWREN active-high) | confirmed | Schematic: direct wiring (no inverters) to N4's RSTB pin and the board's SGM2028-ADJ LDO's EN pin |
| Register map shape (bank-switched, write 0xFF to select) + 8-bit reg/val width | confirmed via sibling drivers, not N4's own text | Two independent NVP6324 drivers' actual bank-select writes, lining up with N4's own datasheet chapter numbering (BANK0/BANK1/BANK2~3/BANK4/BANK20/BANK21) |
| Chip-ID readback (`DEV_ID` = 0xB0 at bank0 reg 0xF4) | confirmed | N4 datasheet |
| Per-input enable/disable (`PD_VCH`, bank0 0x00-0x03) | confirmed | N4 datasheet |
| Video mode/resolution select (`AHD_MD`, bank0 0x08-0x0B) | confirmed -- see camera-fps caveat below | N4 datasheet |
| CSI-2 output data type (0x1E, YUV422 8-bit, bank21 0x38-0x3B) | confirmed | N4 datasheet (also its power-on-reset default) |
| Virtual-channel assignment (auto mode, bank21 0x2E) | confirmed -- see VC caveat below | N4 datasheet |
| CSI-2 lane count + TX start/stop (bank21 0x07 = 0x0F) | confirmed | N4 datasheet bit layout, decoded against a real sibling-driver write of this exact value |
| MIPI PLL / lane bit rate (~1242 Mbps/lane) | best-evidenced, NOT primary-sourced | Two independent NVP6324 drivers agree on 15 of 17 register values; N4's own datasheet leaves this table entirely "TBD" |
| Reset/power-up timing | confirmed (existing delays were already conservative enough) | N4 datasheet (RSTB timing) + SGM2028-ADJ datasheet (LDO settle time) |

Two things worth checking first on real hardware:

- **Frame rate vs. the actual camera heads.** This driver targets a
  fixed 1920x1080@30. N4's own power-on default is 1080p25, and one of
  the camera-head spec sheets MYIR bundled (`QJD6048-2053摄像头规格书.pdf`)
  is a 25fps-only module -- forcing `AHD_MD` to 30P (as
  `mycam004m_init_regs[]` currently does) won't make a 25fps-only
  camera output 30fps. See that table's comment in `mycam004m-regs.h`
  for what to change if that's the camera on your board.
- **Virtual-channel configuration is a known trouble spot for this
  exact chip.** A real TI E2E integration thread about NVP6324 on a TI
  SoC reported total no-video, which TI support traced to VC config.
  Watch this first if streaming comes up empty.

Grep the repo for `TODO` for what's left -- none of it is register-map
related (deliberately-omitted frame-interval ops, the unregistered
"myir" devicetree vendor prefix).

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

This turns out to be doubly justified: the MY-CAM004M board schematic
(`MY-CAM004M/my-cam004m-20230725.pdf`) shows its own `PWRDN` net isn't
wired to anything either -- N4 doesn't have a PWRDN pin at all (only
RSTB), so the module's PWRDN line is a no-connect on this variant, not
just unrouted on BeaglePlay's end.

## Repo layout

- `mycam004m.c` -- the driver.
- `mycam004m.h` -- shared constants/structs (pad indices, fixed target
  format, the `struct mycam004m_reg` register-table entry type).
- `mycam004m-regs.h` -- the register tables, populated from the
  Nextchip N4 datasheet and cross-validated sibling-chip drivers; see
  its top comment for the full citation trail and remaining caveats.
- `Kconfig`, `Makefile` -- see "Building" below.
- `dts/k3-am625-beagleplay-mycam004m.dtso` -- devicetree overlay:
  I2C address, reset/pwren GPIOs, 4 CSI-2 lanes, endpoint link to
  `cdns_csi2rx0`.
- `docs/testing.md` -- `media-ctl`/`v4l2-ctl` commands to verify the
  media graph, format negotiation, and actual 4-stream capture (the
  register tables are populated now, but this hasn't been run against
  real hardware -- see the Status section above).
- `mycam004m-fake.c` -- a second, independent module: a fake capture
  driver that serves 4 static reference images as real `/dev/video*`
  frame buffers, so a calling app's V4L2 code can be tested end-to-end
  without MY-CAM004M hardware, independent of whether the real driver's
  register tables turn out to be right. Not part of the real driver --
  see `docs/fake-driver-testing.md`.
- `tools/gen_fake_frames.py` -- generates a synthetic solid-color/marker-
  square variant of the 4 reference images `mycam004m-fake.c` serves
  (`firmware/mycam004m-fake/cam{1..4}.bin`). The images currently
  committed there are real captured frames from the car's 4 physical
  cameras instead -- see `docs/fake-driver-testing.md` for how they
  were produced and how to regenerate the synthetic version.
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

`dtc` alone isn't enough -- the overlay's `#include
<dt-bindings/gpio/gpio.h>` needs a C preprocessor pass first (this was
actually caught during this driver's own build verification: bare `dtc`
on this file fails with a syntax error on that line). The kernel source
tree (not the build-output `$KDIR` used elsewhere in this doc -- the
actual source, e.g. `.../linux-ti-staging/6.12.57+git/packages-split/
linux-ti-staging-src/board-support/ti-linux-kernel-6.12.57+git-ti` in
this project's Yocto volume) has the `dt-bindings` headers `cpp` needs
and a `scripts/dtc/dtc` binary:

```sh
KSRC=/path/to/kernel/source/tree   # has include/dt-bindings/, not just the build output

aarch64-oe-linux-cpp -nostdinc -undef -x assembler-with-cpp \
    -I "$KSRC/include" -I "$KSRC/arch/arm64/boot/dts" \
    dts/k3-am625-beagleplay-mycam004m.dtso -o /tmp/mycam004m.dts.tmp

"$KSRC/scripts/dtc/dtc" -@ -I dts -O dtb \
    -o k3-am625-beagleplay-mycam004m.dtbo \
    -i "$KSRC/arch/arm64/boot/dts/ti" \
    /tmp/mycam004m.dts.tmp
```

(Substitute your cross-compiler's `cpp` if not using the Yocto
toolchain above.) This was run for real as part of verifying this
driver's register-table update -- see "How this was verified" below.

then apply it however your bootloader/overlay mechanism expects
(U-Boot `fdt apply`, `/boot/overlays`, etc. -- specific to how this
project's BeaglePlay build is set up; see
`~/code/UltimaGC/beagleplay-falcon/NOTES.md` for that side of things,
which this repo intentionally doesn't touch).

### Verifying

See `docs/testing.md` for `media-ctl`/`v4l2-ctl` commands to check the
media graph, format negotiation, and streams -- including what to check
first if streaming doesn't come up clean (the register tables are
populated but untested against real hardware -- see the Status section
above).

### Testing the calling app without real hardware

See `docs/fake-driver-testing.md`. `mycam004m-fake.ko` (built from
this same Makefile) serves 4 static reference images as real
`/dev/video*` frame buffers, so a calling app's open/negotiate/stream
code can be exercised against genuine (if static) delivered pixel
data today, independent of whether the real driver's register tables
turn out to be right on actual hardware.

## How this was verified

Initially written with no MY-CAM004M hardware or MYIR documentation
available, so "verified" here means something specific and limited --
this section covers both that initial pass and the later register-table
update once documentation did turn up:

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
  matching that exact kernel build. Re-run and re-confirmed clean after
  the register-table update described above.
- **The devicetree overlay actually compiles**, `cpp`-preprocessed then
  run through the real kernel source tree's `scripts/dtc/dtc` (see
  "Applying the devicetree overlay" above for why bare `dtc` doesn't
  work on this file). The resulting `.dtbo` was decompiled back to
  source and its property values checked byte-for-byte: `reg = <0x30>`,
  `reset-gpios` pin 11 flags `GPIO_ACTIVE_LOW`, `pwren-gpios` pin 12
  flags `GPIO_ACTIVE_HIGH`, `data-lanes = <1 2 3 4>`, and
  `link-frequencies` decodes to exactly 1242000000 -- all landed
  correctly, not just "the build didn't error."
- **The register tables in `mycam004m-regs.h` were cross-validated
  between independent sources**, not written from a single guess: N4's
  own datasheet where it documents a register directly, and byte-level
  comparison between two independently-written NVP6324 drivers (from
  different companies, targeting different SoCs) where it doesn't (the
  MIPI PLL table) -- 15 of 17 register values matched exactly between
  those two sources. Full citations are inline in `mycam004m-regs.h`.
- **What this does *not* verify**: actual behavior against real
  MY-CAM004M hardware. No board was available for this update either --
  every register value is either primary-sourced or cross-validated
  against other drivers, never hardware-tested. See the Status section
  above for the two specific things (camera frame rate, virtual-channel
  behavior) most worth checking first.
