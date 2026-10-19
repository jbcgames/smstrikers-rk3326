#!/usr/bin/env python3
"""
generate_splash.py
Generates resolution-matched, centered splash images from Logo.jpg
with edge replication (repeating column 0 on left and column -1 on right)
to seamlessly fill 4:3, 16:9, 1:1, 3:2 and custom resolution handheld screens.
"""

import os
import sys
from PIL import Image

def generate_splash(src_path, dst_path, target_w, target_h):
    im = Image.open(src_path).convert('RGB')
    if target_w >= target_h:
        # Scale logo to match target height
        scaled_logo = im.resize((target_h, target_h), Image.Resampling.LANCZOS)
        x_start = (target_w - target_h) // 2
        canvas = Image.new('RGB', (target_w, target_h))
        canvas.paste(scaled_logo, (x_start, 0))
        
        # Left fill: repeat column 0
        if x_start > 0:
            col_first = scaled_logo.crop((0, 0, 1, target_h))
            left_fill = col_first.resize((x_start, target_h), Image.Resampling.NEAREST)
            canvas.paste(left_fill, (0, 0))
            
        # Right fill: repeat column (target_h - 1)
        x_end = x_start + target_h
        if x_end < target_w:
            right_w = target_w - x_end
            col_last = scaled_logo.crop((target_h - 1, 0, target_h, target_h))
            right_fill = col_last.resize((right_w, target_h), Image.Resampling.NEAREST)
            canvas.paste(right_fill, (x_end, 0))
    else:
        # Portrait: Scale logo to match target width
        scaled_logo = im.resize((target_w, target_w), Image.Resampling.LANCZOS)
        y_start = (target_h - target_w) // 2
        canvas = Image.new('RGB', (target_w, target_h))
        canvas.paste(scaled_logo, (0, y_start))
        
        # Top fill: repeat row 0
        if y_start > 0:
            row_first = scaled_logo.crop((0, 0, target_w, 1))
            top_fill = row_first.resize((target_w, y_start), Image.Resampling.NEAREST)
            canvas.paste(top_fill, (0, 0))
            
        # Bottom fill: repeat row (target_w - 1)
        y_end = y_start + target_w
        if y_end < target_h:
            bottom_h = target_h - y_end
            row_last = scaled_logo.crop((0, target_w - 1, target_w, target_w))
            bottom_fill = row_last.resize((target_w, bottom_h), Image.Resampling.NEAREST)
            canvas.paste(bottom_fill, (0, y_end))
            
    canvas.save(dst_path, quality=95)
    print(f"Generated {dst_path} ({target_w}x{target_h})")

def main():
    if len(sys.argv) >= 5:
        src = sys.argv[1]
        dst = sys.argv[2]
        w = int(sys.argv[3])
        h = int(sys.argv[4])
        generate_splash(src, dst, w, h)
        return

    script_dir = os.path.dirname(os.path.abspath(__file__))
    port_dir = os.path.dirname(script_dir)
    src = os.path.join(port_dir, "Logo.jpg")
    if not os.path.exists(src):
        src = os.path.join(os.path.dirname(port_dir), "Logo.jpg")

    resolutions = [
        ("Logo_640x480.jpg", 640, 480),
        ("Logo_4x3.jpg", 640, 480),
        ("Logo_854x480.jpg", 854, 480),
        ("Logo_16x9.jpg", 854, 480),
        ("Logo_960x540.jpg", 960, 540),
        ("Logo_1280x720.jpg", 1280, 720),
        ("Logo_1920x1080.jpg", 1920, 1080),
        ("Logo_720x720.jpg", 720, 720),
        ("Logo_1x1.jpg", 720, 720),
        ("Logo_480x480.jpg", 480, 480),
        ("Logo_480x320.jpg", 480, 320),
        ("Logo_720x480.jpg", 720, 480),
        ("Logo_3x2.jpg", 720, 480),
    ]

    out_dirs = [
        os.path.join(port_dir, "portmaster"),
        os.path.join(port_dir, "smstrikers-port", "portmaster")
    ]
    for d in out_dirs:
        if os.path.exists(d):
            for fname, w, h in resolutions:
                generate_splash(src, os.path.join(d, fname), w, h)

if __name__ == "__main__":
    main()
