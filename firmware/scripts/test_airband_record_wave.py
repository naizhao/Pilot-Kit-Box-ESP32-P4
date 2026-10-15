#!/usr/bin/env python3
"""Run the real recorder against POSIX files, then parse its output independently."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[2]
SDK = Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk')
if SDK.is_dir():
    os.environ.setdefault('SDKROOT', str(SDK))
out = Path(tempfile.mkdtemp(prefix='pk_record_wave_'))
binary = out / 'record'
subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
    '-D_DEFAULT_SOURCE', '-I', str(ROOT/'firmware/test/record_stubs'),
    '-I', str(ROOT/'firmware/test/host_stubs'), '-I', str(ROOT/'firmware/main'),
    str(ROOT/'firmware/test/test_airband_record.c'), str(ROOT/'firmware/main/airband_wav.c'),
    str(ROOT/'firmware/main/airband_audio.c'), '-lm', '-o', str(binary)], check=True)
result = subprocess.run([str(binary)], capture_output=True, text=True, check=True)
match = re.search(r'record files retained in (.+)', result.stdout)
assert match, result.stdout
directory = Path(match.group(1))/'airband'
files = [p for p in directory.glob('*.wav') if not p.name.endswith('.part.wav')]
assert len(files) >= 9, files
for path in files:
    with wave.open(str(path), 'rb') as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) == (1, 2, 16000, 'NONE'), path
        pcm = wav.readframes(wav.getnframes())
        assert len(pcm) == wav.getnframes()*2 == path.stat().st_size-44, path
        assert any(pcm), f'mute erased recording: {path}'
part = directory/'s0000000999-c121500-f121500000-0000000001.part.wav'
expected = (b'RIFF'+(36).to_bytes(4,'little')+b'WAVEfmt '+(16).to_bytes(4,'little')+
    b'\x01\x00\x01\x00'+(16000).to_bytes(4,'little')+(32000).to_bytes(4,'little')+
    b'\x02\x00\x10\x00data'+bytes(4)+b'\xd2\x04\xd3\x04\xd4\x04\xff')
assert part.read_bytes() == expected, part
for session in (999,1000,1001):
    pending = directory/f's{session:010d}-c121500-f121500000-0000000001.part.wav'
    assert pending.exists(), pending
    assert not pending.with_name(pending.name[:-9]+'-interrupted.wav').exists(), pending
    if session != 999:
        data = bytearray(expected[:-1]); data[4:8] = (42 if session==1000 else 99).to_bytes(4,'little'); data[40:44] = (6).to_bytes(4,'little')
        assert pending.read_bytes() == data, pending
print(f'OK: Python wave independently parsed {len(files)} actual recorder files; artifacts: {out}, {directory}')
