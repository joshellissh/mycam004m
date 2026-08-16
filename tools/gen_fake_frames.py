#!/usr/bin/env python3
"""Generate the 4 static reference frames mycam004m-fake.c loads via
request_firmware(). Pure stdlib, no image libraries needed -- every
frame here is axis-aligned solid-color rectangles, so it's built
directly as YUYV bytes rather than rendered and converted.

Output: firmware/mycam004m-fake/cam{1,2,3,4}.bin, each exactly
WIDTH*HEIGHT*2 bytes of raw V4L2_PIX_FMT_YUYV (must match
MYCAM004M_WIDTH/HEIGHT in mycam004m.h -- see the constants below).

Each frame is a distinct solid background color with a black marker
bar containing N white squares (N = camera number, 1-4), so the camera
number is identifiable by eye (or by a script counting squares) even
after a raw-to-viewable conversion that loses color, e.g. grayscale.

Usage:
    python3 tools/gen_fake_frames.py
    # then, on the target:
    #   mkdir -p /lib/firmware/mycam004m-fake
    #   cp firmware/mycam004m-fake/cam*.bin /lib/firmware/mycam004m-fake/
"""
import pathlib

# Must match MYCAM004M_WIDTH / MYCAM004M_HEIGHT in mycam004m.h.
WIDTH = 1920
HEIGHT = 1080

BACKGROUNDS = {
    1: (200, 40, 40),    # red
    2: (40, 180, 70),    # green
    3: (50, 80, 200),    # blue
    4: (210, 170, 30),   # amber
}
BLACK = (16, 16, 16)
WHITE = (235, 235, 235)

# Marker bar geometry (all even, for YUYV 2-pixel macropixel alignment).
BAR_X, BAR_Y = 40, 40
SQUARE = 140
GAP = 40
BAR_H = 220
BAR_W = 4 * SQUARE + 5 * GAP  # room for up to 4 squares + margins


def rgb_to_yuyv_pair(r, g, b):
    """One YUYV macropixel (2 pixels, identical color) as 4 bytes."""
    y = round(0.299 * r + 0.587 * g + 0.114 * b)
    u = round(128 - 0.168736 * r - 0.331264 * g + 0.5 * b)
    v = round(128 + 0.5 * r - 0.418688 * g - 0.081312 * b)
    y, u, v = (max(0, min(255, x)) for x in (y, u, v))
    return bytes((y, u, y, v))


def solid_row(width_px, color):
    assert width_px % 2 == 0
    return rgb_to_yuyv_pair(*color) * (width_px // 2)


def build_frame(cam_number):
    bg = BACKGROUNDS[cam_number]
    bg_row = solid_row(WIDTH, bg)
    frame = bytearray(bg_row * HEIGHT)

    # Marker bar background (black), full BAR_W x BAR_H.
    bar_row = solid_row(BAR_W, BLACK)
    row_bytes = WIDTH * 2
    for y in range(BAR_Y, BAR_Y + BAR_H):
        off = y * row_bytes + BAR_X * 2
        frame[off:off + len(bar_row)] = bar_row

    # cam_number white squares, vertically centered in the bar.
    square_row = solid_row(SQUARE, WHITE)
    sq_y0 = BAR_Y + (BAR_H - SQUARE) // 2
    for i in range(cam_number):
        sq_x0 = BAR_X + GAP + i * (SQUARE + GAP)
        for y in range(sq_y0, sq_y0 + SQUARE):
            off = y * row_bytes + sq_x0 * 2
            frame[off:off + len(square_row)] = square_row

    assert len(frame) == WIDTH * HEIGHT * 2
    return bytes(frame)


def main():
    out_dir = pathlib.Path(__file__).resolve().parent.parent / "firmware" / "mycam004m-fake"
    out_dir.mkdir(parents=True, exist_ok=True)

    for cam_number in sorted(BACKGROUNDS):
        data = build_frame(cam_number)
        path = out_dir / f"cam{cam_number}.bin"
        path.write_bytes(data)
        print(f"wrote {path} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
