#!/usr/bin/env python3
"""Isolated, one-defect-at-a-time behavioral mutations; no source-tree writes."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT/'firmware/main'
TEST = ROOT/'firmware/test'
SDK = Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk')
if SDK.is_dir():
    os.environ.setdefault('SDKROOT',str(SDK))
out = Path(tempfile.mkdtemp(prefix='pk_record_mutations_'))
base = ['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread','-D_DEFAULT_SOURCE',
        '-I',str(TEST/'record_stubs'),'-I',str(TEST/'host_stubs'),'-I',str(MAIN)]

mutations = [
    ('rec_without_permission','record','!s_status.enabled || !s_ready || s_holdoff','!s_ready || s_holdoff'),
    ('rec_overflow_resume','record','!s_status.enabled || !s_ready || s_holdoff','!s_status.enabled || !s_ready'),
    ('rec_glitch_rearms_overflow','record','if (!sq) s_holdoff=false;','s_holdoff=false;'),
    ('rec_ignores_rf','record','const bool gate = rf && sq && pcm && frames;','const bool gate = sq && pcm && frames;'),
    ('rec_ignores_squelch','record','const bool gate = rf && sq && pcm && frames;','const bool gate = rf && pcm && frames;'),
    ('rec_capture_blocks_on_fat','record','    const bool gate = rf && sq && pcm && frames;',
     '    xSemaphoreTake(s_io,portMAX_DELAY);xSemaphoreGive(s_io);\n    const bool gate = rf && sq && pcm && frames;'),
    ('rec_stop_lost','record','else { end_segment(false);s_status.stopping=','else { s_status.stopping='),
    ('rec_restart_while_closing','record','(s_ready && !s_status.stopping)','s_ready'),
    ('rec_unbounded_copy','record','if (frames > PK_AIRBAND_RECORD_BLOCK_SAMPLES)','if (false)'),
    ('rec_init_overwrites_worker','record','    s_status.started=true;',
     '    s_status.started=true;s_status.sd_missing=true;'),
    ('rec_hides_corrupt_fat','record','        if (pk_sdcard_media_error()) s_status.error = true;','        (void)0;'),
    ('rec_unmount_fd_leak','record','    close_segment(true);\n    portENTER_CRITICAL(&s_mux);\n    s_head = s_tail = 0;',
     '    portENTER_CRITICAL(&s_mux);\n    s_head = s_tail = 0;'),
    ('rec_bad_recovery_truncates','record','if (!e && ftruncate(fileno(fp),','if (ftruncate(fileno(fp),'),
    ('rec_recovery_keeps_odd_tail','record','ftruncate(fileno(fp),pk_airband_wav_file_size(&wav))',
     'ftruncate(fileno(fp),(off_t)st.st_size)'),
    ('rec_recovery_ignores_format','record','memcmp(header+8,format,sizeof(format)) == 0',
     'memcmp(header+8,format,8) == 0'),
    ('wav_rate','wav','put_u32(out + 24, PK_AIRBAND_WAV_SAMPLE_RATE);','put_u32(out + 24, 48000);'),
    ('wav_endianness','wav','p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);',
     'p[1] = (uint8_t)value; p[0] = (uint8_t)(value >> 8);'),
    ('wav_real_written_count','wav','w->data_bytes += (uint32_t)written;','w->data_bytes += (uint32_t)(n*2);'),
    ('wav_sync_error_hidden','wav','if (w->io.sync(w->io.ctx) != 0) w->errors |= PK_AIRBAND_WAV_ERR_SYNC;',
     '(void)w->io.sync(w->io.ctx);'),
    ('wav_close_error_hidden','wav','if (w->io.close(w->io.ctx) != 0) w->errors |= PK_AIRBAND_WAV_ERR_CLOSE;',
     '(void)w->io.close(w->io.ctx);'),
    ('wav_checkpoint_position','wav','PK_AIRBAND_WAV_HEADER_BYTES + w->data_bytes) != 0',
     'PK_AIRBAND_WAV_HEADER_BYTES) != 0'),
    ('wav_recovery_odd_size','wav','((uint32_t)actual_size - PK_AIRBAND_WAV_HEADER_BYTES) & ~1u;',
     '((uint32_t)actual_size - PK_AIRBAND_WAV_HEADER_BYTES);'),
    ('wav_rotation_consumes_all','wav','*consumed_samples += written / 2;',
     '*consumed_samples += sample_count;'),
    ('sd_format_skips_barrier','sd','for (int i = 0; i < s_pre_unmount_cb_n; ++i) s_pre_unmount_cb[i]();','(void)0;'),
    ('sd_format_reuses_generation','sd','        s_mount_generation++;\n        sd_set_state(PK_SD_MOUNTED);',
     '        sd_set_state(PK_SD_MOUNTED);'),
]
for name,kind,old,new in mutations:
    source = MAIN/({'record':'airband_record.c','wav':'airband_wav.c','sd':'pk_sdcard.c'}[kind])
    text = source.read_text()
    assert text.count(old)==1,(name,text.count(old))
    changed=out/(name+'.c');changed.write_text(text.replace(old,new))
    binary=out/name
    if kind=='wav':
        cmd=base+[str(TEST/'test_airband_wav.c'),str(changed),'-o',str(binary)]
    else:
        testfile=TEST/('test_airband_record.c' if kind=='record' else 'test_sd_format_barrier.c')
        harness=out/(name+'_test.c')
        include='../main/'+source.name
        testtext=testfile.read_text();assert testtext.count(include)==1
        harness.write_text(testtext.replace(include,str(changed)))
        cmd=base+[str(harness)]
        if kind=='record':cmd += [str(MAIN/'airband_wav.c'),str(MAIN/'airband_audio.c'),'-lm']
        cmd += ['-o',str(binary)]
    subprocess.run(cmd,check=True,capture_output=True,text=True)
    try:
        result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=5)
    except subprocess.TimeoutExpired:
        assert name=='rec_capture_blocks_on_fat',f'unexpected hang: {name}'
        print('KILLED',name,'(producer waited on deliberately stalled FAT)')
        continue
    assert result.returncode!=0, f'SURVIVED: {name}'
    assert 'FAIL' in result.stdout+result.stderr, f'not a behavioral failure: {name}'
    print('KILLED',name)
print(f'OK: {len(mutations)} isolated mutations, retained at {out}')
