# Verifying the media graph and streams

This assumes the module is built and loaded (see `../README.md`) and the
`k3-am625-beagleplay-mycam004m.dtso` overlay is applied.

**Status check before you start**: `mycam004m_init_regs[]` and
`mycam004m_csi_output_regs[]` in `../mycam004m-regs.h` are currently
empty, and the per-input enable/disable functions in `../mycam004m.c`
are stubs. This means the steps below can validate the *media graph,
format negotiation, and routing* right now, without the MYIR
programming guide -- but `VIDIOC_STREAMON` on any of the capture nodes
will not produce real image data yet (the decoder chip is never
actually told to output anything). Expect DMA/frame timeouts at the
final streaming step until the register tables are filled in. Getting
that far without an error before that point is the actual goal of this
document.

The commands below are the standard TI j721e-csi2rx / Cadence csi2rx
workflow, adapted to this driver's topology. They have not been run
against real MY-CAM004M hardware (no register table yet to make the
chip output anything) -- if a command's exact spelling doesn't match
your `media-ctl`/`v4l2-ctl` version, `media-ctl -p`'s own output is the
source of truth for real pad/entity names on your system.

## 1. Confirm the driver bound

```sh
dmesg | grep -i mycam004m
# expect: "mycam004m 4-00XX: probed at I2C address 0x30, 4 data lanes,
#          link-freq 400000000 Hz (TODO: unverified placeholder, ...)"

i2cdetect -y 4
# wkup_i2c0 is i2c4 on BeaglePlay -- confirm the actual address here
# against the TODO placeholder in the DT overlay (0x30).
```

## 2. Print the media graph

```sh
media-ctl -d /dev/media0 -p
```

Expect to see three entities in the chain, in addition to the four
`ti-csi2rx0-context` capture video nodes:

```
mycam004m <video-mux-like, 4 sink pads + 1 source pad>
    -> csi-bridge (cdns_csi2rx0 / cdns,csi2rx)
        -> ticsi2rx (ti_csi2rx0 / ti,j721e-csi2rx-shim)
            -> /dev/videoN  (x4, one per context/virtual channel)
```

If `mycam004m` doesn't appear at all, check `dmesg` for probe failures
first (most likely: GPIO/regmap/endpoint-parse errors from
`mycam004m.c`'s probe path -- all of those log via `dev_err_probe()`).

## 3. Set format and routing on the decoder

The default routing installed by `mycam004m_init_state()` already maps
AHD input *N* -> source pad stream *N* 1:1, so this step is mostly a
way to confirm (rather than establish) the topology:

```sh
# Format on each sink pad (identity 1:1, YUYV 1920x1080):
for cam in 0 1 2 3; do
  media-ctl -d /dev/media0 \
    --set-v4l2-subdev-format "'mycam004m':${cam}/0 [fmt:YUYV8_1X16/1920x1080]"
done

# Confirm routing (should already show 4 active 1:1 routes):
media-ctl -d /dev/media0 --get-v4l2-subdev-routing "'mycam004m'"
```

## 4. Confirm the format propagates to the CSI-2 bridge and capture nodes

```sh
media-ctl -d /dev/media0 \
  --set-v4l2-subdev-format "'csi-bridge':0 [fmt:YUYV8_1X16/1920x1080]"

# Each of the 4 capture nodes should independently report the format:
for n in 0 1 2 3; do
  v4l2-ctl -d /dev/video$n --get-fmt-video
done
```

## 5. Confirm virtual-channel assignment

```sh
v4l2-ctl -d /dev/video0 --list-subdev-mbus-codes
media-ctl -d /dev/media0 --get-v4l2-subdev-frame-desc "'mycam004m':4"
```

The VC numbers reported here come straight from
`mycam004m_get_frame_desc()` / `mycam004m_vc_for_cam()` in
`../mycam004m.c` -- currently AHD input *N* -> VC *N*. This is a TODO
placeholder (see the comment there and in `../mycam004m-regs.h`); if
the real chip assigns VCs differently, this is where you'll see it once
it's fixed.

## 6. Streaming test (expected to fail/timeout until the register tables are filled in)

```sh
for n in 0 1 2 3; do
  v4l2-ctl -d /dev/video$n \
    --set-fmt-video=width=1920,height=1080,pixelformat=YUYV \
    --stream-mmap --stream-count=1 --stream-to=/tmp/cam$n.raw &
done
wait
```

Once `mycam004m-regs.h` has a real init/CSI-output table and the
per-input enable/disable stubs in `mycam004m.c` do real register
writes, this should produce four `1920x1080` YUYV raw frames, one per
camera input. Until then, expect this step to hang/timeout waiting for
DMA that the decoder was never actually told to send.
