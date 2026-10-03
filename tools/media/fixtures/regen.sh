#!/usr/bin/env bash
# Rebuilds the three MP3 fixtures. Only needed when they change: the tests use the committed files, so CI
# needs no ffmpeg. mono22 and stereo44 are 1 s of 440 Hz, tone880 is 12 s of 880 Hz for the Music boot check.
set -euo pipefail
cd "$(dirname "$0")"
ffmpeg -v error -y -f lavfi -i sine=frequency=440:duration=1 -ac 1 -ar 22050 -q:a 9 mono22.mp3
ffmpeg -v error -y -f lavfi -i sine=frequency=440:duration=1 -ac 2 -ar 44100 -q:a 9 stereo44.mp3
ffmpeg -v error -y -f lavfi -i sine=frequency=880:duration=12 -ac 1 -ar 22050 -q:a 9 tone880.mp3
