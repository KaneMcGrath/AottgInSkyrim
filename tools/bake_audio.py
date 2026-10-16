# SPDX-License-Identifier: GPL-3.0-only
"""Package HERO cues as mono PCM16 WAVs for Skyrim positional playback."""
import array
import pathlib
import sys
import wave

source, destination = map(pathlib.Path, sys.argv[1:])
destination.mkdir(parents=True, exist_ok=True)
for name in ('hook-launch', 'blade-swing', 'ahss-fire', 'blade-hit', 'death'):
    with wave.open(str(source / (name + '.wav')), 'rb') as clip:
        channels, width, rate = clip.getnchannels(), clip.getsampwidth(), clip.getframerate()
        if width != 2 or channels not in (1, 2) or clip.getcomptype() != 'NONE':
            raise ValueError(f'{name}: expected mono/stereo PCM16')
        samples = array.array('h', clip.readframes(clip.getnframes()))
    if sys.byteorder != 'little':
        samples.byteswap()
    if channels == 2:
        samples = array.array('h', (int((samples[i] + samples[i + 1]) / 2)
                                  for i in range(0, len(samples), 2)))
    if sys.byteorder != 'little':
        samples.byteswap()
    with wave.open(str(destination / (name + '.wav')), 'wb') as clip:
        clip.setparams((1, 2, rate, 0, 'NONE', 'not compressed'))
        clip.writeframes(samples.tobytes())
    print(f'{name}: mono PCM16, {rate} Hz, {len(samples)} samples')
