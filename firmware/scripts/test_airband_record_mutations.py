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
    ('rec_short_sd_jitter_queue','record','#include "airband_record.h"',
     '#include "airband_record.h"\n#undef PK_AIRBAND_RECORD_QUEUE_BLOCKS\n#define PK_AIRBAND_RECORD_QUEUE_BLOCKS 64u'),
    ('rec_source_gap_splits_file','record',
     's_source_gap_run >= PK_AIRBAND_RECORD_SOURCE_GAP_LIMIT_BLOCKS',
     's_source_gap_run >= 1u'),
    ('rec_sustained_source_gap_ignored','record',
     's_source_gap_run >= PK_AIRBAND_RECORD_SOURCE_GAP_LIMIT_BLOCKS',
     's_source_gap_run >= UINT32_MAX'),
    ('rec_source_gap_recovery_not_reset','record',
     '} else if (gate) s_source_gap_run=0;',
     '} else if (gate) (void)0;'),
    ('rec_empty_queue_interrupt_lost','record',
     'if (s_pending_interrupted_segment == s_io_segment) {',
     'if (false && s_pending_interrupted_segment == s_io_segment) {'),
    ('rec_pcm_not_coalesced','record','#define RECORD_WRITE_BATCH_BLOCKS 32u',
     '#define RECORD_WRITE_BATCH_BLOCKS 1u'),
    ('rec_without_permission','record','!s_status.enabled || !s_ready || s_holdoff','!s_ready || s_holdoff'),
    ('rec_overflow_resume','record','!s_status.enabled || !s_ready || s_holdoff','!s_status.enabled || !s_ready'),
    ('rec_glitch_rearms_overflow','record','if (!sq) s_holdoff=false;','s_holdoff=false;'),
    ('rec_ignores_rf','record','const bool gate = rf && sq && pcm && frames;','const bool gate = sq && pcm && frames;'),
    ('rec_ignores_squelch','record','const bool gate = rf && sq && pcm && frames;','const bool gate = rf && pcm && frames;'),
    ('rec_capture_blocks_on_fat','record','    const bool gate = rf && sq && pcm && frames;',
     '    xSemaphoreTake(s_io,portMAX_DELAY);xSemaphoreGive(s_io);\n    const bool gate = rf && sq && pcm && frames;'),
    ('rec_stop_lost','record','else { end_segment(false);s_status.stopping=','else { s_status.stopping='),
    ('rec_restart_while_closing','record','(mounted && !s_status.stopping)','mounted'),
    ('rec_disabled_prepares_storage','record_access',
     's_status.enabled || s_status.stopping || s_tail != s_head','true'),
    ('rec_enable_requires_prepared_deadlock','record_access',
     '(mounted && !s_status.stopping)','(mounted && s_ready && !s_status.stopping)'),
    ('rec_preparation_failure_busy_retry','record_access',
     's_prepared_mount != mount && esp_timer_get_time() >= s_prepare_retry_at',
     's_prepared_mount != mount'),
    ('rec_failed_recovery_repeated','record_access','    s_prepared_mount = mount;',
     '    (void)mount;s_prepared_mount = 0;'),
    ('rec_new_mount_keeps_backoff','record_access',
     '        s_prepare_retry_at = 0;','        (void)0;'),
    ('rec_preparation_failure_is_missing_card','record_access',
     's_status.sd_missing = !pk_sdcard_is_mounted();','s_status.sd_missing = !s_ready;'),
    ('rec_unbounded_copy','record','if (frames > PK_AIRBAND_RECORD_BLOCK_SAMPLES)','if (false)'),
    ('rec_init_overwrites_worker','record','    s_status.started=true;',
     '    s_status.started=true;s_status.sd_missing=true;'),
    ('rec_hides_corrupt_fat','record','        if (pk_sdcard_media_error()) s_status.error = true;','        (void)0;'),
    ('rec_unmount_fd_leak','record','    close_segment(true);\n    portENTER_CRITICAL(&s_mux);\n    s_head = s_tail = 0;',
     '    portENTER_CRITICAL(&s_mux);\n    s_head = s_tail = 0;'),
    ('rec_preparation_rewrites_header','record','FILE *fp = fopen(path,"rb");',
     'FILE *fp = fopen(path,"r+b"); if(fp) { (void)fwrite("FAIL",1,4,fp); }'),
    ('rec_preparation_truncates_tail','record','FILE *fp = fopen(path,"rb");',
     'FILE *fp = fopen(path,"r+b"); if(fp) { (void)ftruncate(fileno(fp),44); }'),
    ('rec_preparation_renames_pending','record',
     '    return read_ok && close_ok;',
     '    char final[320]; (void)publish_final(path,true,final,sizeof(final));\n    return read_ok && close_ok;'),
    ('rec_preparation_publishes_valid_checkpoint','record',
     '    return read_ok && close_ok;',
     '    if(header[40]==6 && header[4]==42) { char final[320]; (void)publish_final(path,true,final,sizeof(final)); }\n    return read_ok && close_ok;'),
    ('rec_inspection_hides_read_failure','record_access','const bool read_ok = ferror(fp) == 0;',
     'const bool read_ok = (ferror(fp),true);'),
    ('rec_inspection_hides_close_failure','record_access','const bool close_ok = fclose(fp) == 0;',
     'const bool close_ok = (fclose(fp),true);'),
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
    source = MAIN/({'record':'airband_record.c','record_access':'airband_record.c',
                   'wav':'airband_wav.c','sd':'pk_sdcard.c'}[kind])
    text = source.read_text()
    assert text.count(old)==1,(name,text.count(old))
    changed=out/(name+'.c');changed.write_text(text.replace(old,new))
    binary=out/name
    if kind=='wav':
        cmd=base+[str(TEST/'test_airband_wav.c'),str(changed),'-o',str(binary)]
    else:
        testfile=TEST/({'record':'test_airband_record.c',
                       'record_access':'test_airband_record_access.c',
                       'sd':'test_sd_format_barrier.c'}[kind])
        harness=out/(name+'_test.c')
        include='../main/'+source.name
        testtext=testfile.read_text();assert testtext.count(include)==1
        harness.write_text(testtext.replace(include,str(changed)))
        cmd=base+[str(harness)]
        if kind in ('record','record_access'):cmd += [str(MAIN/'airband_wav.c'),str(MAIN/'airband_audio.c'),'-lm']
        if kind=='sd':cmd += [str(MAIN/'pk_sd_transaction_safety.c')]
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
