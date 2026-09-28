#!/usr/bin/env python3
"""One-at-a-time lifecycle mutations exercised by real production consumers."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / 'firmware/main'
TEST = ROOT / 'firmware/test'
SDK = Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk')
if SDK.is_dir():
    os.environ.setdefault('SDKROOT', str(SDK))
out = Path(tempfile.mkdtemp(prefix='pk_sd_mutations_'))
base = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
        '-D_DEFAULT_SOURCE', '-I', str(TEST / 'record_stubs'),
        '-I', str(TEST / 'host_stubs'), '-I', str(MAIN),
        '-I', str(ROOT / 'firmware/components/adsb_link_codec')]

variants = []
for filename, signature in [
    ('pk_syslog.c', 'static void syslog_task(void *arg)'),
    ('power_log.c', 'static void power_log_task(void *arg)'),
    ('rf1090_log.c', 'static void rf1090_log_task(void *arg)'),
    ('rf1090_log.c', 'void rf1090_log_dump(void)'),
    ('demo_track_sd.c', 'static void demo_sd_task(void *arg)'),
    ('config_diaglog.c', 'void pk_diaglog_clear_files(void)'),
    ('config_diaglog.c', 'long pk_diaglog_size_bytes(void)'),
]:
    source = (MAIN / filename).read_text()
    assert source.count(signature) == 1
    start = source.index(signature)
    end = source.index('\n}\n', start) + 3
    old = source[start:end]
    assert source.count(old) == 1
    assert old.count('pk_sdcard_io_acquire()') == 1
    # Revert precisely this consumer to its old check-without-lifetime-pin
    # behavior. Its matching releases are removed with that single lease.
    new = old.replace('pk_sdcard_io_acquire()', 'pk_sdcard_is_mounted()')
    new = new.replace('pk_sdcard_io_release();', '(void)0;')
    variants.append((signature.split('(')[0].split()[-1] + '_unleased',
                     filename, source.replace(old, new), 'test_sd_consumers.c'))

for name, filename, old, new, harness in [
    ('format_skips_inflight', 'pk_sdcard.c',
     '    sd_wait_io_idle();\n    /* Same lifetime barrier',
     '    /* Same lifetime barrier', 'test_sd_consumers.c'),
    ('unmount_skips_inflight', 'pk_sdcard.c',
     '    sd_wait_io_idle();\n    for (int i = 0;',
     '    for (int i = 0;', 'test_sd_consumers.c'),
    ('admission_accepts_while_formatting', 'pk_sdcard.c',
     'bool admitted = s_state == PK_SD_MOUNTED;',
     'bool admitted = true;', 'test_sd_consumers.c'),
    ('retune_keeps_raw_window', 'airband_task.c',
     '            pk_airband_audio_stats_reset(&audio);\n',
     '', 'test_airband_retune.c'),
]:
    source = (MAIN / filename).read_text()
    assert source.count(old) == 1, (name, source.count(old))
    variants.append((name, filename, source.replace(old, new), harness))

for name, filename, source, harness_name in variants:
    changed = out / (name + '.c')
    changed.write_text(source)
    harness = (TEST / harness_name).read_text()
    old = '../main/' + filename
    assert harness.count(old) == 1
    harness = harness.replace(old, str(changed))
    harness = harness.replace('../main/', str(MAIN) + '/')
    test_path = out / (name + '_test.c')
    test_path.write_text(harness)
    binary = out / name
    cmd = base + [str(test_path)]
    if harness_name == 'test_airband_retune.c':
        cmd += [str(MAIN / f) for f in ['airband_bk4819.c', 'airband_audio.c',
                'airband_pcm16.c', 'airband_am_gain.c', 'airband_frequency_input.c',
                'airband_speaker_policy.c']]
    subprocess.run(cmd + ['-lm', '-o', str(binary)], check=True,
                   capture_output=True, text=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    assert result.returncode != 0, 'SURVIVED: ' + name
    assert 'FAIL' in result.stdout + result.stderr, 'not behavioral: ' + name
    print('KILLED', name)
print(f'OK: {len(variants)} isolated mutations; retained at {out}')
