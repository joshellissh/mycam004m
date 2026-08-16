# Testing the calling app against static reference images

`mycam004m-fake.ko` is a second, independent kernel module in this
repo (`mycam004m-fake.c`). It is **not** part of the real mycam004m
driver and is never loaded on a real board -- it exists purely so a
calling application's V4L2 capture code can be exercised against real,
delivered frame buffers before MY-CAM004M hardware or its register
tables (see `mycam004m-regs.h`) are available.

It registers 4 independent `/dev/videoX` capture devices, each
replaying one static YUYV reference image, at the real driver's target
format (1920x1080 YUYV) and framerate (30fps). There is no I2C, GPIO,
devicetree, or media-controller code involved -- see the comment at
the top of `mycam004m-fake.c` for why this can't instead be a "fake
mode" of the real driver (short version: the real driver never touches
pixel data even when complete -- there's no injection point).

**Status: load-tested on real BeaglePlay hardware.** All 4 devices
register, stream, and deliver frames that are byte-identical to their
reference images (`cmp` confirmed, all 4 cameras) -- see "What actually
happened on real hardware" at the bottom of this doc, including one
real bug (now fixed) that only showed up at runtime, not at compile
time.

## 1. Generate the reference images

```sh
python3 tools/gen_fake_frames.py
```

Writes `firmware/mycam004m-fake/cam{1,2,3,4}.bin` -- raw YUYV422
frames, each a distinct solid background color with a black marker bar
containing N white squares (N = camera number), so the camera number
is identifiable by eye. Pure Python stdlib, no dependencies. Edit
`BACKGROUNDS`/geometry constants in that script to change them.

## 2. Build

Same Kbuild flow as the real driver (see README.md's "Building"
section for the full Docker/KDIR/toolchain command) -- this Makefile
now produces both `.ko` files in one `make` invocation:

```sh
make -C "$KDIR" M=$(pwd) ARCH=arm64 CROSS_COMPILE=aarch64-oe-linux- modules
ls mycam004m.ko mycam004m-fake.ko
```

Build-verified against the real BeaglePlay Yocto kernel tree the same
way as `mycam004m.ko`: `W=1`, zero warnings, `modinfo` confirms
`depends: videodev,videobuf2-common,videobuf2-v4l2,videobuf2-vmalloc`
and a `vermagic` matching that kernel build.

## 3. Install firmware and load, on the target

```sh
mkdir -p /lib/firmware/mycam004m-fake
cp firmware/mycam004m-fake/cam*.bin /lib/firmware/mycam004m-fake/
modprobe videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc
insmod mycam004m-fake.ko
dmesg | tail -8   # should show 4 "camN: ... ready as /dev/videoX" lines
```

The explicit `modprobe` matters: `insmod` (unlike `modprobe`) does not
resolve a module's own dependencies, so if `videodev`/`videobuf2-*`
aren't already loaded (e.g. right after a fresh boot, before any other
V4L2 driver has pulled them in), a plain `insmod mycam004m-fake.ko`
fails with a page of "Unknown symbol" errors. Confirmed on real
hardware -- see below.

If `/lib/firmware` is read-only on your target (it is on the stock
BeaglePlay image), point the firmware loader at a writable directory
instead of copying there:

```sh
mkdir -p /some/writable/path/mycam004m-fake
cp firmware/mycam004m-fake/cam*.bin /some/writable/path/mycam004m-fake/
echo /some/writable/path > /sys/module/firmware_class/parameters/path
```

That sysfs knob adds an extra search directory ahead of the default
`/lib/firmware` -- no rootfs remount needed. Reset it to `""` when
you're done if you'd rather not leave a nonstandard search path active.

If a firmware file is missing or the wrong size, that's a hard load
failure with a `dev_err` naming the exact file and expected size --
by design, so a broken/incomplete fake setup doesn't silently produce
some but not all 4 devices.

## 4. Point the calling app at it

Resolve stable `/dev/mycam/cam1`..`cam4` symlinks so the app never
needs to care which backend is active:

```sh
scripts/select-camera-backend.sh fake
```

(`scripts/select-camera-backend.sh real` does the same resolution
against TI's real `j721e-csi2rx` capture nodes once `mycam004m.ko` is
loaded and the devicetree overlay applied -- see the script's header
comment for exactly how it tells the two backends' nodes apart, even
if both modules happen to be loaded at once.)

The calling app should open `/dev/mycam/cam1` through `cam4` --
nothing else in its V4L2 code needs to change between test and real.

## 5. Verify real frame delivery (not just ioctl success)

If `v4l2-ctl` is available (it is not on the stock BeaglePlay image --
no `v4l-utils` package installed):

```sh
v4l2-ctl -d /dev/mycam/cam1 --all
v4l2-ctl -d /dev/mycam/cam1 \
    --set-fmt-video=width=1920,height=1080,pixelformat=YUYV \
    --stream-mmap --stream-count=1 --stream-to=/tmp/cam1-captured.raw
```

Otherwise, a plain `read()` works too -- `mycam004m-fake.c` wires
`vb2_fop_read` and sets `V4L2_CAP_READWRITE`, and vb2's read-mode
fallback drives the same REQBUFS/QBUF/STREAMON/DQBUF path internally.
This is what was actually used to verify against real hardware:

```sh
dd if=/dev/mycam/cam1 of=/tmp/cam1-captured.raw bs=4147200 count=1
```

(`4147200` = `1920 * 1080 * 2` bytes, one YUYV frame -- get it wrong
and `dd` will short-read or block; see `mycam004m.h`.)

Either way, finish with:

```sh
cmp /tmp/cam1-captured.raw firmware/mycam004m-fake/cam1.bin && \
    echo "MATCH: captured frame is byte-identical to the reference image"
```

That `cmp` is the actual proof this is different from the media-graph
smoke test in `docs/testing.md`: it confirms a real buffer, with real
(if static) pixel content, made it all the way through
`VIDIOC_STREAMON`/`DQBUF` to the app -- not just that the ioctls
returned success.

Repeat for `cam2`..`cam4` (each has a distinct reference image, so
you're also confirming the app correctly keeps 4 independent streams
separate, not accidentally reading the same device 4 times).

## What actually happened on real hardware

This module was load-tested end to end on a real BeaglePlay
(`6.12.57-ti-01316-g31b07ab8dfbc`), over SSH, after building against
that exact kernel (matching `vermagic`). Worth recording, since none of
this showed up in the `W=1` compile that came before it:

- **One real bug, found only at runtime.** The first `insmod` oopsed
  inside `v4l2_device_register()` -- a NULL-pointer dereference on
  `dev->driver->name`, hit because `v4l2_dev->name[0]` was empty *and*
  the module's `platform_device` has no bound `platform_driver` (it
  only exists as a `struct device` for firmware-loader/dev_err
  attribution), so the kernel's "derive a default name" fallback
  dereferenced a NULL `dev->driver`. Fixed in `mycam004m_fake_init()`
  by setting `mycam004m_fake_v4l2_dev.name` explicitly before calling
  `v4l2_device_register()`, so that fallback path is never reached.
  This is exactly the class of bug a clean compile can't catch --
  worth remembering if this module is ever modified without re-testing
  on real hardware.
- **A crashed `insmod` can leave the module stuck.** Because the oops
  happened mid-`init_module`, the kernel never ran its normal
  init-failure unwind: `mycam004m_fake` stayed in `lsmod` with a
  phantom reference count, and `rmmod` refused ("Module is in use").
  The only clean recovery found was a full reboot -- nothing else
  short of that released it. Test iteratively in small steps for
  exactly this reason.
- **After a real bug fix, `rmmod` was confirmed clean**: exits 0, all
  4 `/dev/videoX` nodes disappear, no dangling references.
- **The backend-switch script originally used `v4l2-ctl -D` for
  driver-name disambiguation; that doesn't work on this board** (no
  `v4l-utils`). Rewritten to use only
  `/sys/class/video4linux/videoX/name`, confirmed against the live
  devices (e.g. `video0`'s `name` reads `"mycam004m-fake context 0"`);
  see `scripts/select-camera-backend.sh`'s header comment.
