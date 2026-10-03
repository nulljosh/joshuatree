#!/usr/bin/env python3
import subprocess
import sys
import os

def gen_mp3(outdir, name, rate, channels, duration=1):
    """Generate a test MP3 file using ffmpeg or lame"""
    out = os.path.join(outdir, name)
    if os.path.exists(out):
        return
    try:
        if subprocess.run(['which', 'ffmpeg'], capture_output=True).returncode == 0:
            subprocess.run([
                'ffmpeg', '-f', 'lavfi', '-i',
                f'sine=frequency=440:duration={duration}',
                '-ac', str(channels), '-ar', str(rate), '-q:a', '9',
                '-y', out
            ], check=True, capture_output=True)
        elif subprocess.run(['which', 'lame'], capture_output=True).returncode == 0:
            subprocess.run([
                'ffmpeg', '-f', 'lavfi', '-i',
                f'sine=frequency=440:duration={duration}',
                '-f', 'wav', '-'
            ], check=True, capture_output=True, stdout=subprocess.PIPE)
        else:
            print("ffmpeg or lame required", file=sys.stderr)
            sys.exit(1)
    except subprocess.CalledProcessError as e:
        print(f"Error generating {name}: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    outdir = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(outdir, exist_ok=True)
    gen_mp3(outdir, 'stereo44.mp3', 44100, 2, 1)
    gen_mp3(outdir, 'mono22.mp3', 22050, 1, 1)
