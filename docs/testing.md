# Verifying the media graph and streams

This assumes the module is built and loaded (see `../README.md`) and the
`k3-am625-beagleplay-mycam004m.dtso` overlay is applied.

**Status check before you start**: `mycam004m_init_regs[]` and
`mycam004m_csi_output_regs[]` in `../mycam004m-regs.h` are now
populated (from the Nextchip N4 datasheet plus cross-validated
sibling-chip drivers -- see the README's Status section), and the
per-input enable/disable and CSI-2 TX start/stop functions in
`../mycam004m.c` do real register writes. So the steps below, including
streaming, are no longer expected to fail *by design* -- but **none of
this has been run against real MY-CAM004M hardware**, so a failure at
any step could equally mean a wrong register value as it could mean
something environmental. See the README's Status section for the two
specific things (camera frame rate, virtual-channel behavior) most
likely to bite first.

The commands below are the standard TI j721e-csi2rx / Cadence csi2rx
workflow, adapted to this driver's topology. They have not been run
against real MY-CAM004M hardware -- if a command's exact spelling
doesn't match your `media-ctl`/`v4l2-ctl` version, `media-ctl -p`'s own
output is the source of truth for real pad/entity names on your system.

## 1. Confirm the driver bound

```sh
dmesg | grep -i mycam004m
# expect: "mycam004m 4-0030: found N4 decoder: DEV_ID 0xb0, REV_ID 0x00"
# followed by: "mycam004m 4-0030: probed at I2C address 0x30, 4 data
#              lanes, link-freq 1242000000 Hz"
# If DEV_ID readback fails or reads back something other than 0xb0,
# probe fails right there (see mycam004m_check_chip_id() in
# mycam004m.c) -- that's the first thing to check against I2C address
# 0x30 below before looking any further.

i2cdetect -y 4
# wkup_i2c0 is i2c4 on BeaglePlay -- confirm 0x30 actually shows up.
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
`../mycam004m.c` -- AHD input *N* -> VC *N*, which N4's auto-VC-insertion
mode (bank 0x21 reg 0x2E, written in `mycam004m_csi_output_regs[]`) is
supposed to produce automatically. This is the single most
likely thing to be wrong on real hardware -- see the VC caveat in the
README's Status section (a real TI E2E thread about this exact chip
reported total no-video traced to VC config) -- so if step 6 below
hangs or produces garbled/misrouted frames, check here first.

## 6. Streaming test

```sh
for n in 0 1 2 3; do
  v4l2-ctl -d /dev/video$n \
    --set-fmt-video=width=1920,height=1080,pixelformat=YUYV \
    --stream-mmap --stream-count=1 --stream-to=/tmp/cam$n.raw &
done
wait
```

This should produce four `1920x1080` YUYV raw frames, one per camera
input -- `mycam004m-regs.h` now has a real init/CSI-output table and
the per-input enable/disable functions in `mycam004m.c` do real
register writes, so the decoder should actually be told to send data
at this point. Since none of this has been tested against real
hardware, a hang/timeout here is still possible -- if so, check (in
order): actual camera signal present on the AHD inputs, the DEV_ID
readback in step 1, then the MIPI PLL/lane-rate register block at the
top of `mycam004m_csi_output_regs[]` (flagged there as the
lowest-confidence value, sourced from sibling-chip drivers rather than
N4's own datasheet).
