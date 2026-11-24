#!/usr/bin/env python3
"""Prepare phone JPEGs for SkyFrame's small circular 480x480 LCD.
Requires Pillow: python -m pip install pillow
Usage: python tools/prepare_photos.py ./input_photos F:/photos   (F: = the microSD card)
Output: 480x480 baseline JPEGs, EXIF rotated and centered, no metadata retained.
"""
import argparse
from pathlib import Path
from PIL import Image, ImageOps

def convert_one(src: Path, dst: Path):
    with Image.open(src) as im:
        im = ImageOps.exif_transpose(im)
        im = ImageOps.fit(im.convert('RGB'), (480,480), method=Image.Resampling.LANCZOS, centering=(.5,.5))
        im.save(dst, format='JPEG', quality=85, optimize=True, progressive=False)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path)
    p.add_argument('destination',type=Path)
    a=p.parse_args()
    if not a.source.is_dir():p.error('Source must be a directory')
    a.destination.mkdir(parents=True,exist_ok=True)
    count=0
    for f in sorted(a.source.iterdir()):
        if f.suffix.lower() not in ('.jpg','.jpeg','.png','.webp'):continue
        out=a.destination/(f.stem+'.jpg')
        try:
            convert_one(f,out)
        except Exception as e:   # unreadable/unsupported file: skip it, keep going
            print(f'{f.name}: skipped ({e})')
            continue
        print(f'{f.name} -> {out.name} ({out.stat().st_size:,} bytes)')
        count+=1
    print(f'Prepared {count} images. Copy the photos directory to FAT32 microSD.')
if __name__=='__main__':main()
