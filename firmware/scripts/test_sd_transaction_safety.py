#!/usr/bin/env python3
"""Exercise the actual SDK transaction body, with deterministic host events."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("IDF_PATH", Path.home() / ".espressif/v6.0.1/esp-idf"))
DRIVER = SDK / "components/esp_driver_sdmmc/src/sd_trans_sdmmc.c"

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "pk_sd_transaction_safety.h"
#define CONFIG_PM_ENABLE 1
#define SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE 1
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 4
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 8
#define SD_HOST_SLOT_STATE_READY 1
#define SD_SWITCH_VOLTAGE 11
#define MMC_APP_CMD 55
#define SCF_CMD_READ 0x0040
#define MMC_READ_BLOCK_SINGLE 17
#define MMC_READ_BLOCK_MULTIPLE 18
#define MMC_WRITE_BLOCK_SINGLE 24
#define MMC_WRITE_BLOCK_MULTIPLE 25
#define SCF_WAIT_BUSY 0x2000
#define SDMMC_IDLE 0
#define SDMMC_SENDING_CMD 1
#define SDMMC_SENDING_VOLTAGE_SWITCH 2
#define portMAX_DELAY -1
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_RETURN_ON_FALSE(c,e,...) do { if(!(c))return(e); }while(0)
#define ESP_RETURN_ON_ERROR(expr,...) do { int e=(expr);if(e)return e; }while(0)
#define ESP_GOTO_ON_ERROR(expr,label,...) do { ret=(expr);if(ret)goto label; }while(0)
#define ESP_LOGE(...) ((void)0)
#define __containerof(ptr,type,member) ((type*)((char*)(ptr)-offsetof(type,member)))
typedef void *sd_host_slot_handle_t;
typedef int sdmmc_hw_cmd_t;
typedef int sdmmc_req_state_t;
typedef struct { int dummy; } sd_host_sdmmc_event_t;
typedef struct { void *mutex, *pm_lock; int cur_slot_id; bool is_app_cmd; } controller_t;
typedef struct { int drv; controller_t *ctlr; int slot_id; struct { int freq_state, freq_hz; } freq; } sd_host_sdmmc_slot_t;
typedef struct { int opcode; void *data; size_t datalen, buflen, blklen; unsigned flags,arg,timeout_ms; int error; } sdmmc_command_t;
static int event_error, command_error, cache_error, pm_release_error, pm_acquire_error, pm_releases;
static int c2m_error, start_error;
static bool aligned = true, busy_cleared = true;
static int commands, event_calls, c2m, m2c, lock_depth;
static bool fault_on_take;
static void xSemaphoreTake(void *lock,int ticks) {(void)lock;(void)ticks;assert(lock_depth++==0);if(fault_on_take)pk_sd_transaction_complete(0,true,ESP_ERR_TIMEOUT);}
static void xSemaphoreGive(void *lock) {(void)lock;assert(--lock_depth==0);}
static int esp_pm_lock_acquire(void *p) {(void)p;return pm_acquire_error;}
static int esp_pm_lock_release(void *p) {(void)p;pm_releases++;return pm_release_error;}
static int sd_host_slot_set_card_clk(sd_host_sdmmc_slot_t *p) {(void)p;return 0;}
static int sd_host_slot_get_real_freq(sd_host_sdmmc_slot_t *p,int *freq) {*freq=p->freq.freq_hz/1000;return 0;}
static int sd_host_slot_set_bus_width(sd_host_sdmmc_slot_t *p) {(void)p;return 0;}
static int sd_host_slot_set_bus_sampling_mode(sd_host_sdmmc_slot_t *p) {(void)p;return 0;}
static int sd_host_set_delay_phase(sd_host_sdmmc_slot_t *p) {(void)p;return 0;}
static int sd_host_set_delay_line(sd_host_sdmmc_slot_t *p) {(void)p;return 0;}
static void sd_host_handle_idle_state_events(sd_host_sdmmc_slot_t *p) {(void)p;}
static void handle_voltage_switch_stage1(sd_host_sdmmc_slot_t *p,sdmmc_command_t *c) {(void)p;(void)c;}
static int make_hw_cmd(sdmmc_command_t *c) {(void)c;return 0;}
static bool sd_host_check_buffer_alignment(sd_host_sdmmc_slot_t *p,void *data,size_t n) {(void)p;(void)data;(void)n;return aligned;}
static int esp_cache_msync(void *data,size_t n,int flags) {(void)data;(void)n;if(flags==4){c2m++;return c2m_error;}assert(flags==8);m2c++;return cache_error;}
static void sd_host_dma_prepare(sd_host_sdmmc_slot_t *p,void *data,size_t n,size_t blk) {(void)p;(void)data;(void)n;(void)blk;}
static int sd_host_slot_start_command(sd_host_sdmmc_slot_t *p,int cmd,unsigned arg) {(void)p;(void)cmd;(void)arg;commands++;return start_error;}
static int handle_event(sd_host_sdmmc_slot_t *p,sdmmc_command_t *c,int *state,sd_host_sdmmc_event_t *events) {(void)p;(void)events;event_calls++;c->error=command_error;*state=0;return event_error;}
static bool wait_for_busy_cleared(sd_host_sdmmc_slot_t *p,unsigned ticks) {(void)p;(void)ticks;return busy_cleared;}
TRANSACTION
int main(int argc,char **argv) {
    assert(argc==2);
    controller_t controller={.mutex=&controller,.pm_lock=&controller};
    sd_host_sdmmc_slot_t slot={.ctlr=&controller,.freq={.freq_state=1,.freq_hz=20000000}};
    unsigned char data[512]={0};
    sdmmc_command_t cmd={.opcode=17,.data=data,.datalen=512,.buflen=512,.blklen=512,.flags=SCF_CMD_READ};
    int expected=0, expected_m2c=1, expected_c2m=1, expected_commands=1; bool fault=false;
    if(strcmp(argv[1],"before_mount"))pk_sd_transaction_arm();
    if(!strcmp(argv[1],"read_timeout")){event_error=ESP_ERR_TIMEOUT;expected=event_error;fault=true;}
    else if(!strcmp(argv[1],"write_timeout")){cmd.opcode=24;cmd.flags=0;event_error=ESP_ERR_TIMEOUT;expected=event_error;fault=true;expected_m2c=0;}
    else if(!strcmp(argv[1],"read_multi_timeout")){cmd.opcode=18;event_error=ESP_ERR_TIMEOUT;expected=event_error;fault=true;}
    else if(!strcmp(argv[1],"write_multi_timeout")){cmd.opcode=25;cmd.flags=0;event_error=ESP_ERR_TIMEOUT;expected=event_error;fault=true;expected_m2c=0;}
    else if(!strcmp(argv[1],"both_errors")){event_error=ESP_ERR_TIMEOUT;cache_error=ESP_FAIL;expected=event_error;fault=true;}
    else if(!strcmp(argv[1],"cache_error")){cache_error=ESP_FAIL;expected=cache_error;}
    else if(!strcmp(argv[1],"cache_c2m_not_supported")){c2m_error=ESP_ERR_NOT_SUPPORTED;expected=c2m_error;expected_m2c=0;expected_commands=0;}
    else if(!strcmp(argv[1],"cache_m2c_not_supported")){cache_error=ESP_ERR_NOT_SUPPORTED;expected=cache_error;}
    else if(!strcmp(argv[1],"cache_c2m_invalid_arg")){c2m_error=ESP_ERR_INVALID_ARG;expected=c2m_error;expected_m2c=0;expected_commands=0;}
    else if(!strcmp(argv[1],"invalid_alignment")){aligned=false;expected=ESP_ERR_INVALID_ARG;expected_c2m=expected_m2c=expected_commands=0;}
    else if(!strcmp(argv[1],"invalid_size")){cmd.datalen=6;expected=ESP_ERR_INVALID_SIZE;expected_c2m=expected_m2c=expected_commands=0;}
    else if(!strcmp(argv[1],"command_error")){command_error=ESP_FAIL;fault=true;}
    else if(!strcmp(argv[1],"command_cache_error")){command_error=ESP_FAIL;cache_error=ESP_ERR_INVALID_ARG;fault=true;}
    else if(!strcmp(argv[1],"write_success")){cmd.flags=0;expected_m2c=0;}
    else if(!strcmp(argv[1],"slot1")){slot.slot_id=1;event_error=ESP_ERR_TIMEOUT;expected=event_error;}
    else if(!strcmp(argv[1],"command_only")){cmd.data=NULL;event_error=ESP_ERR_TIMEOUT;expected=event_error;expected_c2m=expected_m2c=0;}
    else if(!strcmp(argv[1],"protocol_probe")){cmd.opcode=6;event_error=ESP_ERR_TIMEOUT;expected=event_error;}
    else if(!strcmp(argv[1],"before_mount")){event_error=ESP_ERR_TIMEOUT;expected=event_error;}
    else if(!strcmp(argv[1],"pm_second_error")){event_error=ESP_ERR_TIMEOUT;pm_release_error=ESP_FAIL;expected=event_error;fault=true;}
    else if(!strcmp(argv[1],"pm_acquire_error")){pm_acquire_error=ESP_FAIL;expected=pm_acquire_error;expected_c2m=expected_m2c=expected_commands=0;}
    else if(!strcmp(argv[1],"pm_release_error")){pm_release_error=ESP_FAIL;expected=pm_release_error;}
    else if(!strcmp(argv[1],"queued_after_failure")){fault_on_take=true;event_error=ESP_ERR_TIMEOUT;expected=ESP_ERR_INVALID_STATE;fault=true;expected_c2m=expected_m2c=expected_commands=0;}
    else if(!strcmp(argv[1],"command_crc_error")){command_error=ESP_ERR_INVALID_CRC;fault=true;}
    else if(!strcmp(argv[1],"busy_timeout")){cmd.flags|=SCF_WAIT_BUSY;busy_cleared=false;expected=ESP_ERR_TIMEOUT;fault=true;}
    else if(!strcmp(argv[1],"start_timeout")){start_error=ESP_ERR_TIMEOUT;expected=start_error;expected_m2c=0;fault=true;}
    else assert(!strcmp(argv[1],"zero_read"));
    int result=sd_host_slot_sdmmc_do_transaction(&slot.drv,&cmd);
    assert(result==expected && cmd.error==command_error && lock_depth==0);
    assert(m2c==expected_m2c && c2m==expected_c2m && commands==expected_commands);
    assert(pm_releases==((pm_acquire_error || fault_on_take)?0:1));
    assert(pk_sd_transaction_fault()==(fault?(start_error?start_error:event_error?event_error:command_error?command_error:ESP_ERR_TIMEOUT):ESP_OK));
    if(fault){
        int before=commands;
        cmd.error=0;cmd.flags=0;cmd.opcode=24;
        assert(sd_host_slot_sdmmc_do_transaction(&slot.drv,&cmd)==ESP_ERR_INVALID_STATE);
        assert(commands==before && lock_depth==0);
        cmd.data=NULL;event_error=command_error=cache_error=c2m_error=pm_release_error=pm_acquire_error=start_error=0;fault_on_take=false;busy_cleared=true;
        assert(sd_host_slot_sdmmc_do_transaction(&slot.drv,&cmd)==0 && commands==before+1);
        cmd.data=data;slot.slot_id=1;
        assert(sd_host_slot_sdmmc_do_transaction(&slot.drv,&cmd)==0 && commands==before+2);
    }
    return 0;
}
'''


def module():
    spec = importlib.util.spec_from_file_location("sd_overlay", ROOT / "scripts/prepare_sd_transaction_safety.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def body(source):
    return source[source.index("esp_err_t sd_host_slot_sdmmc_do_transaction("):]


def compile_driver(source, directory):
    path = Path(directory)
    cfile, exe = path / "transaction.c", path / "transaction"
    cfile.write_text(HARNESS.replace("TRANSACTION", body(source)))
    safety = ROOT / "main/pk_sd_transaction_safety.c"
    if not safety.exists():
        # Before implementation this test still exercises the real broken SDK.
        (path / "pk_sd_transaction_safety.h").write_text(
            '#include "esp_err.h"\nstatic void pk_sd_transaction_arm(void){}\n'
            'static int pk_sd_transaction_fault(void){return 0;}\n')
    (path / "sdkconfig.h").write_text("/* Host fixture for official ESP-IDF error definitions. */\n")
    command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
               "-I", str(path), "-I", str(SDK / "components/esp_common/include"),
               "-I", str(ROOT / "test/host_stubs"), "-I", str(ROOT / "main"), str(cfile)]
    if safety.exists():
        command.append(str(safety))
    command += ["-o", str(exe)]
    built = subprocess.run(command, text=True, capture_output=True)
    if built.returncode:
        raise AssertionError(built.stderr)
    return exe


class SdTransactionSafetyTest(unittest.TestCase):
    def source(self, directory):
        generator = ROOT / "scripts/prepare_sd_transaction_safety.py"
        if not generator.exists():
            return DRIVER.read_text()
        output = Path(directory) / "sd_trans_sdmmc.c"
        module().prepare_driver(DRIVER, output)
        return output.read_text()

    def test_real_sdk_read_write_errors_and_cache_directions(self):
        with tempfile.TemporaryDirectory(prefix="pk-sd-transaction-") as directory:
            exe = compile_driver(self.source(directory), directory)
            for case in ("read_timeout", "write_timeout", "read_multi_timeout", "write_multi_timeout", "both_errors", "cache_error", "command_error", "command_cache_error",
                         "cache_c2m_not_supported", "cache_m2c_not_supported", "cache_c2m_invalid_arg", "invalid_alignment", "invalid_size",
                         "write_success", "slot1", "command_only", "protocol_probe", "before_mount", "pm_second_error", "pm_acquire_error", "pm_release_error",
                         "queued_after_failure", "command_crc_error", "busy_timeout", "start_timeout", "zero_read"):
                with self.subTest(case=case):
                    result = subprocess.run([str(exe), case], text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_generator_does_not_modify_sdk_and_rejects_unknown_source(self):
        original = DRIVER.read_bytes()
        with tempfile.TemporaryDirectory(prefix="pk-sd-generator-") as directory:
            p = Path(directory); source=p/"unknown.c"; output=p/"overlay.c"
            source.write_bytes(original+b"\n");output.write_bytes(b"old")
            with self.assertRaisesRegex(ValueError,"Unsupported ESP-IDF SDMMC"):
                module().prepare_driver(source,output)
            self.assertEqual(output.read_bytes(),b"old")
            with self.assertRaisesRegex(ValueError,"must not overwrite"):
                module().prepare_driver(DRIVER,DRIVER)
            module().prepare_driver(DRIVER,output)
            timestamp=output.stat().st_mtime_ns
            module().prepare_driver(DRIVER,output)
            self.assertEqual(output.stat().st_mtime_ns,timestamp)
        self.assertEqual(DRIVER.read_bytes(),original)

    def test_behavior_mutations_are_individually_rejected(self):
        with tempfile.TemporaryDirectory(prefix="pk-sd-mutation-") as directory:
            source = self.source(directory)
            overlay = module()
            variants = (
                (overlay.CACHE_NEW, overlay.CACHE_OLD, "read_timeout"),
                ("if (ret == ESP_OK && cmdinfo->error == ESP_OK) ret = cache_ret;",
                 "ret = cache_ret;", "both_errors"),
                ("if (cmdinfo->data && (cmdinfo->flags & SCF_CMD_READ)) {",
                 "if (cmdinfo->data) {", "write_success"),
                (overlay.NOTIFY, "    (void)card_error;\n", "read_timeout"),
                ("                               card_error);",
                 "                               ((void)card_error, ret != ESP_OK ? ret : cmdinfo->error));", "cache_c2m_not_supported"),
                (overlay.CAPTURE_CARD_ERROR,"", "read_timeout"),
                ("        card_error = ret;\n        goto out;", "        goto out;", "start_timeout"),
                (overlay.LOCK_ADMISSION, "", "queued_after_failure"),
                ("if (pm_lock_acquired)", "if ((void)pm_lock_acquired, slot_ctx->ctlr->pm_lock)", "pm_acquire_error"),
                ("if (ret == ESP_OK && cmdinfo->error == ESP_OK) ret = pm_ret;",
                 "ret = pm_ret;", "pm_second_error"),
            )
            for index, (old, new, case) in enumerate(variants):
                with self.subTest(case=case, mutation=index):
                    self.assertEqual(source.count(old), 1)
                    variant = source.replace(old,new,1)
                    path = Path(directory)/str(index);path.mkdir()
                    exe = compile_driver(variant,path)
                    result = subprocess.run([str(exe),case],text=True,capture_output=True)
                    self.assertNotEqual(result.returncode,0,"Mutation survived")

    def configure(self, path, entries):
        fixture = r'''
cmake_minimum_required(VERSION 3.16)
project(sd_overlay_test C)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/other.c" "int other(void){return 0;}\n")
add_library(sd_driver STATIC @ENTRIES@ "${CMAKE_CURRENT_BINARY_DIR}/other.c")
function(idf_component_get_property out component property)
    if(property STREQUAL "COMPONENT_LIB")
        set(${out} sd_driver PARENT_SCOPE)
    else()
        set(${out} "@DIR@" PARENT_SCOPE)
    endif()
endfunction()
function(idf_build_get_property out property)
    if(property STREQUAL "BUILD_DIR")
        set(${out} "${CMAKE_CURRENT_BINARY_DIR}" PARENT_SCOPE)
    else()
        set(${out} "@PYTHON@" PARENT_SCOPE)
    endif()
endfunction()
include("@CMAKE@")
pk_enable_sd_transaction_safety()
get_target_property(actual sd_driver SOURCES)
set(expected "${CMAKE_CURRENT_BINARY_DIR}/sd_transaction_safety/sd_trans_sdmmc.c;${CMAKE_CURRENT_BINARY_DIR}/other.c")
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Wrong SD source replacement: ${actual}")
endif()
'''
        for token,value in {"@ENTRIES@":" ".join(f'"{x}"' for x in entries),
                            "@DIR@":str(DRIVER.parent.parent),"@PYTHON@":sys.executable,
                            "@CMAKE@":str(ROOT/"cmake/sd_transaction_safety.cmake")}.items():
            fixture=fixture.replace(token,value)
        path.mkdir();(path/"CMakeLists.txt").write_text(fixture)
        return subprocess.run(["cmake","-S",str(path),"-B",str(path/"build")],text=True,capture_output=True)

    def test_cmake_replaces_only_expected_sdk_source_and_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="pk-sd-cmake-") as directory:
            for index,entries in enumerate(([str(DRIVER)],["src/sd_trans_sdmmc.c"],[],[str(DRIVER),str(DRIVER)])):
                with self.subTest(entries=entries):
                    result=self.configure(Path(directory)/str(index),entries)
                    if len(entries)==1:
                        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    else:
                        self.assertNotEqual(result.returncode,0)
                        self.assertIn("Expected exactly one SDMMC transaction translation unit",result.stderr)

    def test_lifecycle_behavior_mutations_are_individually_rejected(self):
        original = (ROOT/"main/pk_sdcard.c").read_text()
        harness = (ROOT/"test/test_sd_transaction_lifecycle.c").read_text()
        variants = (
            ("    host.check_buffer_alignment = sd_dma_buffer_alignment;\n", ""),
            ("    pk_sd_transaction_arm();\n", ""),
            ("    admitted = admitted && pk_sd_transaction_fault() == ESP_OK;\n", ""),
            ("if (data_error != ESP_OK)", "if (false)"),
            ("    if (pk_sd_transaction_fault() != ESP_OK) return false;\n", ""),
            ("    sd_wait_io_idle();\n    for (int i = 0;", "    for (int i = 0;"),
            ("    if (state == PK_SD_MOUNTED && pk_sd_transaction_fault() != ESP_OK)", "    if (false)"),
            ("return s_media_error || pk_sd_transaction_fault() != ESP_OK;", "return s_media_error;"),
            ("                 pk_sd_transaction_fault() == ESP_OK && s_total_bytes != 0;", "                 s_total_bytes != 0;"),
            # Mutate one boundary at a time; the real detector's mount/GPIO/LDO
            # counts prove the eight-attempt budget, not a source keyword.
            ("    if (s_mount_fail_streak >= SD_INIT_ATTEMPT_LIMIT) return false;\n", ""),
            ("    if (s_mount_fail_streak >= SD_INIT_ATTEMPT_LIMIT) return false;\n",
             "    if (s_mount_fail_streak > SD_INIT_ATTEMPT_LIMIT) return false;\n"),
            ("    s_mount_fail_streak++;\n", ""),
            ("#define SD_INIT_ATTEMPT_LIMIT        8", "#define SD_INIT_ATTEMPT_LIMIT        1000"),
            ("    s_mount_fail_streak = 0;        /* 成功挂载必须清零退避（§4-2） */\n", ""),
            ("    s_probe_exhausted = true;\n", ""),
            ("    if (!sd_power_on_locked()) {\n        sd_mount_failed_locked();\n",
             "    if (!sd_power_on_locked()) {\n        s_mount_fail_streak--;\n        sd_mount_failed_locked();\n"),
            ("    s_mount_fail_streak++;\n    sd_set_info(0, 0);\n    if (!sd_power_on_locked()) {\n        sd_mount_failed_locked();\n        return false;\n    }\n",
             "    sd_set_info(0, 0);\n    if (!sd_power_on_locked()) {\n        sd_mount_failed_locked();\n        return false;\n    }\n    s_mount_fail_streak++;\n"),
            ("            s_pwr_ctrl = NULL;\n            return false;",
             "            s_pwr_ctrl = NULL;\n            s_mount_fail_streak--;\n            return false;"),
            ("    for (uint32_t i = 3; i < streak; i++) {", "    for (uint32_t i = 2; i < streak; i++) {"),
            ("    s_probe_period_ms = SD_PROBE_PERIOD_MS;\n", ""),
            ("    s_mount_fail_streak++;\n", "    s_mount_fail_streak = 1;\n"),
            ("        sd_power_off_locked();\n        sd_mount_failed_locked();",
             "        sd_mount_failed_locked();"),
        )
        with tempfile.TemporaryDirectory(prefix="pk-sd-lifecycle-mutation-") as directory:
            path=Path(directory)
            for index,(old,new) in enumerate(variants):
                with self.subTest(mutation=index):
                    self.assertEqual(original.count(old),1)
                    source=path/f"sd{index}.c";source.write_text(original.replace(old,new,1))
                    self.assertEqual(harness.count('../main/pk_sdcard.c'),1)
                    test=path/f"test{index}.c";test.write_text(harness.replace('../main/pk_sdcard.c',str(source),1))
                    exe=path/f"run{index}"
                    compiled=subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-O2","-pthread",
                         "-I",str(ROOT/"test/record_stubs"),"-I",str(ROOT/"test/host_stubs"),"-I",str(ROOT/"main"),
                         str(test),str(ROOT/"main/pk_sd_transaction_safety.c"),"-o",str(exe)],text=True,capture_output=True)
                    self.assertEqual(compiled.returncode,0,compiled.stderr)
                    result=subprocess.run([str(exe)],text=True,capture_output=True,timeout=10)
                    self.assertNotEqual(result.returncode,0,"Lifecycle mutation survived")


if __name__ == "__main__":
    unittest.main()
