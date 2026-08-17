# Implementation notes for UltimaGC's camera feed

For whoever wires `ultima-app` (in `~/code/UltimaGC`) up to this
driver. Written from this repo because it's the source of truth for
the device contract; `ultima-app/src/camerafeed.cpp` and
`cameraview.cpp` are read (not modified) to ground these notes in the
app's actual existing architecture, not generic V4L2 advice.

## The device contract

Four independent, ordinary V4L2 capture devices — not one device with
4 sub-streams the app has to demux. Whatever muxing happens (real
driver: 4 CSI-2 virtual channels off one physical decoder; fake
driver: 4 independent `vb2` queues) is already resolved by the time
`/dev/videoX` exists. From the app's side each camera is just another
`/dev/videoN` you `open()`, same as the current single grabber.

Stable paths, backend-independent:

```
/dev/mycam/cam1
/dev/mycam/cam2
/dev/mycam/cam3
/dev/mycam/cam4
```

These are symlinks maintained by `scripts/select-camera-backend.sh
real|fake` in this repo (run once, outside the app, before or at
system startup — see "Fake vs. real, and switching between them"
below). **The app should never open `/dev/videoN` directly** — always
these four paths — so it never needs to know or care which backend is
active.

Format is fixed and non-negotiable on both backends:

| | value |
|---|---|
| pixel format | `V4L2_PIX_FMT_YUYV` |
| width x height | 1920 x 1080 |
| bytesperline | 3840 (exactly `width*2`, no stride padding) |
| sizeimage | 4,147,200 |
| field | `V4L2_FIELD_NONE` |
| framerate | 30fps (fake: exactly; real: targeted via register config, untested on hardware -- see caveat below) |

`VIDIOC_S_FMT` is answered by coercion, not negotiation — both drivers
hand back this exact format regardless of what's requested (see
`mycam004m_fake_try_fmt_vid_cap()` / `mycam004m_fake_s_fmt_vid_cap()`
in `mycam004m-fake.c`, and the real driver's `mycam004m_set_fmt()` for
the same philosophy). Confirmed on real hardware:
`bytesperline == width*2` always holds for the fake backend
(load-tested, see `docs/fake-driver-testing.md`) — the real backend's
`mycam004m_set_fmt()` reports the same fixed format via identical logic,
but that specific claim (like everything about the real backend) remains
unverified against actual MY-CAM004M hardware.

**Framerate caveat for the real backend**: `mycam004m-regs.h` explicitly
programs the decoder for 1080p30 (its power-on default is actually
1080p25). That's correct *if* the AHD camera heads feeding MY-CAM004M
are 30fps-capable — but MYIR's own bundled camera-head spec sheet for
one of the modules they ship (`QJD6048-2053`, a panoramic/fisheye head)
states a fixed 25fps with no 30fps option. If that's the physical
camera on the target board, the real backend will actually be
delivering 25fps frames mislabeled as 30fps until `mycam004m.h`/
`mycam004m-regs.h` are updated to match — worth confirming which camera
head is on the board before `CameraFeed` timing assumptions (if any)
get built around 30fps specifically.

`device_caps` is `V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
V4L2_CAP_READWRITE` on both backends — no second/metadata node to
discriminate against, unlike the current UVC grabber (see
`camerafeed.cpp`'s `VIDIOC_QUERYCAP` comment about
`V4L2_CAP_DEVICE_CAPS`). That check can stay as-is; it'll just always
pass here.

## What changes in `CameraFeed`

Almost nothing, structurally. `CameraFeed` already takes a device path
in its constructor
(`CameraFeed(const QString &device = "/dev/video0", ...)`), already
mmaps N buffers via `VIDIOC_REQBUFS`/`QUERYBUF`/`QBUF`, already reads
back the driver-granted `bytesperline` instead of assuming `width*2`
(`tryOpen()`'s comment about not walking off the mmap'd region), and
already drains-and-converts-only-the-newest buffer in `onReadable()`.
All of that is format-agnostic and works unchanged against these
devices.

What actually needs to change:

1. **`kRequestedWidth`/`kRequestedHeight`** (`camerafeed.cpp:29-30`) —
   currently `720`/`480` for the UVC grabber. Since format is coerced
   here regardless of what's requested, the exact requested value
   doesn't functionally matter, but set them to `1920`/`1080` so the
   log line and any code reading these before the first successful
   negotiation reflects reality.
2. **Four instances, not one.** Construct 4 `CameraFeed`s, one per
   `/dev/mycam/camN`. `active`/`streaming`/`failed`/`frameWidth`/
   `frameHeight` are already per-instance `Q_PROPERTY`s, so this is a
   QML-side instantiation question (4 `CameraFeed { device:
   "/dev/mycam/cam1" }`-style objects, or a `QML ListModel`/loop of 4),
   not a C++ architecture change.
3. **`kNumBuffers = 4`** (`camerafeed.h:96`) is probably fine to leave
   as-is — it's buffer *count*, unrelated to resolution — but each
   buffer is now 4,147,200 bytes vs. the grabber's 720x480 YUYV
   (691,200 bytes), i.e. **6x the mmap footprint per buffer, times 4
   cameras** = real memory to account for. Worth an explicit gut-check
   on the target's available RAM rather than assuming it's fine.

## The one real risk worth flagging: CPU conversion cost

`convertYUYVToRGB32()` runs on the GUI thread today (deliberately —
see `camerafeed.h`'s `currentFrame()` comment on why that's safe
*only* because capture+conversion and `CameraView::paint()` are
serialized around the same thread). At 1920x1080 that's ~6x the pixel
count of the current 720x480 grabber feed, **times 4 cameras** — on
the order of 24x today's YUYV->RGB32 conversion cost, all still
proposed to run on the GUI thread.

This project's own comments already treat the *current, single-camera*
conversion cost as tight enough to matter (`onReadable()`'s comment
about not multiplying conversion cost, and the "measure first-Qt-frame
to the millisecond" framing in `camerafeed.h`). A 24x increase is not
a rounding error on that budget and deserves a real look before
assuming the existing GUI-thread-conversion architecture just scales —
options if it doesn't (not a recommendation, a menu):
worker-thread/QtConcurrent conversion per feed (reintroduces the
threading-safety question `currentFrame()`'s comment already flags),
downscaling before conversion if the display size is smaller than
1920x1080 anyway, only converting the feed(s) actually visible in a
quad/360 layout rather than all 4 unconditionally, or a GPU-side YUYV
shader instead of CPU `convertYUYVToRGB32()`.

## `CameraView`: reuse the existing pattern, don't improvise a new one

`cameraview.h`'s comment records two hardware-verified crashes
(SIGSEGV, then SIGBUS) from a hand-managed `QSGSimpleTextureNode`
approach on the real BeaglePlay/PowerVR target — neither reproduced on
the macOS dev build. The fix was `QQuickPaintedItem` +
`FramebufferObject` render target, which hands texture/FBO lifetime
entirely to Qt.

For 4 simultaneous feeds, the strong recommendation is: **4 instances
of the existing `CameraView`**, each with its own `feed` property, laid
out in QML (grid/quad, whatever the UI calls for) — not a new
multi-texture or multi-viewport rendering path. That crash history was
paid for on real hardware with no symbolized Qt libs to debug against;
re-deriving a custom scenegraph approach for 4 feeds risks paying it
again, on the same target, for the same reason.

## Fake vs. real, and switching between them

```sh
# from this repo, against the target (see docs/fake-driver-testing.md
# for the full build/load sequence):
scripts/select-camera-backend.sh fake   # or: real
```

This is an **operational step, run before the app starts** (or
whenever the backend needs to change) — not something `ultima-app`
should invoke itself. The app's only job is to always open
`/dev/mycam/cam1..4`; which backend answers those paths is entirely
outside its concern, which was the whole point of building the
symlink layer this way.

Right now, `real` will resolve TI's actual `j721e-csi2rx` nodes correctly
(load-tested naming/pad assumptions aside — see the script's header
comment on the one thing there that's still an assumption). Streaming
against them is no longer a guaranteed hang: `mycam004m-regs.h`'s
register tables are populated (from the Nextchip N4 datasheet plus
cross-validated sibling-chip drivers — see the main README's Status
section), and the driver now actually tells the decoder to power up
each channel and transmit. **None of that has been run against real
MY-CAM004M hardware, though** — build-tested and cross-source-validated
is not the same as hardware-verified, so a hang or garbled/misrouted
frames on `real` is still a live possibility, not something to rule
out. If it happens, this is app-side-innocent either way (there's
nothing `ultima-app` can do about a wrong decoder register value), but
worth distinguishing a real driver/hardware issue from an app bug
before chasing it as the latter:

1. `dmesg | grep mycam004m` — probe should log `found N4 decoder:
   DEV_ID 0xb0, ...`. If that line is missing or DEV_ID isn't `0xb0`,
   it's an I2C/hardware problem below this driver, not a streaming
   issue.
2. If probe succeeds but `VIDIOC_STREAMON`/`DQBUF` still hangs, the
   most likely culprits are (in order): the MIPI PLL/lane-rate register
   block (flagged in `mycam004m-regs.h` as sourced from sibling-chip
   drivers, not N4's own datasheet — the single lowest-confidence value
   in the whole table), or virtual-channel configuration (a real TI
   E2E integration thread for this exact chip reported total no-video
   traced to VC config — see the README's Status section).

`fake` is fully load-tested end-to-end today (see
`docs/fake-driver-testing.md`'s "What actually happened on real
hardware") and remains the thing to develop/test app logic against
regardless of `real`'s status — it's the only backend with an actual
hardware-verified track record.

## Suggested verification order

1. `scripts/select-camera-backend.sh fake`, confirm `/dev/mycam/cam1..4`
   exist (this repo's own testing already confirms the devices
   themselves work — this step is just confirming the symlinks).
2. Point one `CameraFeed` at `/dev/mycam/cam1`, confirm a real
   (static) image renders through the existing single-feed path
   unchanged, with the new 1920x1080 request constants.
3. Expand to 4 concurrent `CameraFeed`/`CameraView` pairs, confirm all
   4 distinct reference images (each has a different background color
   + N white squares — see `tools/gen_fake_frames.py`) render
   correctly and in the right screen positions — this is a strong
   signal against accidentally cross-wiring which feed maps to which
   `CameraView`, since a swapped `cam2`/`cam3` would be immediately
   visible by color/square-count rather than needing pixel inspection.
4. Only then measure the real GUI-thread conversion cost against the
   render budget (see "risk" section above) before deciding whether it
   needs addressing.
5. Once real MY-CAM004M hardware is available: `scripts/select-camera-backend.sh
   real`, repeat steps 1-3 against actual camera input instead of the
   static references. This is the first real-hardware test of anything
   in `mycam004m-regs.h`'s register tables — see the "Fake vs. real"
   section above for what to check first if it doesn't come up clean.
