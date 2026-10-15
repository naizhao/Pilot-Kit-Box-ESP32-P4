#!/usr/bin/env python3
"""Exercise real SDK sector dispatch with P4 heap regions and DMA eligibility."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
SDK=Path(os.environ.get("IDF_PATH",Path.home()/".espressif/v6.0.1/esp-idf"))


def function(source, signature):
    start=source.index(signature)
    return source[start:source.index("\n}\n",start)+3]


HARNESS=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#define SOC_DMA_LOW 0x4ff00000
#define SOC_DMA_HIGH 0x4ffc0000
#define SOC_IRAM0_ADDRESS_LOW SOC_DMA_LOW
#define SOC_IRAM0_ADDRESS_HIGH SOC_DMA_HIGH
#define SOC_DRAM_FLASH_ADDRESS_LOW 0x40000000
#define SOC_DRAM_PSRAM_ADDRESS_HIGH 0x4c000000
#define CACHE_LL_ID_ALL 3
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_SPIRAM 2
#define SDMMC_DMA_ALIGNMENT 4
#define SD_SLOT 0
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 4
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 8
#define ESP_CACHE_MSYNC_FLAG_TYPE_DATA 16
#define ESP_CACHE_MSYNC_FLAG_TYPE_INST 32
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED 2
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE 1
#define SOC_CACHE_WRITEBACK_SUPPORTED 1
#define ESP_RETURN_ON_FALSE_ISR(c,e,...) do{if(!(c))return (e);}while(0)
#define TAG "SDK fixture"
static void mock_log(const char *tag,const char *fmt,...){(void)tag;(void)fmt;}
#define ESP_LOGD(...) mock_log(__VA_ARGS__)
#define ESP_LOGE(...) mock_log(__VA_ARGS__)
#define ESP_EARLY_LOGV(...) mock_log(__VA_ARGS__)
#define MIN(a,b) ((a)<(b)?(a):(b))
typedef int esp_err_t;
typedef int sd_host_sdmmc_slot_t;
typedef struct {struct {size_t sector_size;} csd;struct {
    int slot;
    bool (*check_buffer_alignment)(int,const void *,size_t);
    void *dma_aligned_buffer;
    size_t unaligned_multi_block_rw_max_chunk_size;
} host;} sdmmc_card_t;
static int transfers,allocations,copies;
static void *transferred;
static size_t transferred_length;
static bool fail_allocate;
static bool esp_ptr_external_ram(const void *p){uintptr_t a=(uintptr_t)p;return a>=0x48000000&&a<0x4c000000;}
static bool esp_ptr_dma_ext_capable(const void *p){return esp_ptr_external_ram(p);}
DMA_POINTER
static int esp_cache_get_alignment(unsigned caps,size_t *alignment){(void)caps;*alignment=64;return 0;}
SDK_ALIGNMENT
static bool sdmmc_host_check_buffer_alignment(int slot,const void *buf,size_t size){(void)slot;return sd_host_check_buffer_alignment(NULL,buf,size);}
PROJECT_ALIGNMENT
CACHE_MAP
typedef enum {CACHE_TYPE_DATA,CACHE_TYPE_INSTRUCTION} cache_type_t;
static int s_spinlock;
static bool cache_hal_vaddr_to_cache_level_id(uint32_t addr,size_t n,uint32_t *level,uint32_t *id){return cache_ll_vaddr_to_cache_level_id(addr,n,level,id);}
static uint32_t cache_hal_get_cache_line_size(uint32_t level,cache_type_t type){(void)level;(void)type;return 64;}
static void s_acquire_mutex_from_task_context(void){}
static void s_release_mutex_from_task_context(void){}
static void esp_os_enter_critical_safe(void *lock){(void)lock;}
static void esp_os_exit_critical_safe(void *lock){(void)lock;}
static bool cache_hal_invalidate_addr(uint32_t addr,size_t n){unsigned level,id;return cache_ll_vaddr_to_cache_level_id(addr,n,&level,&id);}
static void s_c2m_ops(uint32_t addr,size_t n){assert(cache_hal_invalidate_addr(addr,n));}
CACHE_SYNC
static void *heap_caps_malloc(size_t n,unsigned caps){assert(caps==MALLOC_CAP_DMA);allocations++;return fail_allocate?NULL:(void*)0x4ff00000;}
static size_t heap_caps_get_allocated_size(void *p){(void)p;return 512;}
static void mock_free(void *p){assert(p==(void*)0x4ff00000);}
#define free mock_free
static void *mock_copy(void *dst,const void *src,size_t n){assert(n==512);assert(dst&&src);copies++;return dst;}
#define memcpy mock_copy
static int sdmmc_read_sectors_dma(sdmmc_card_t *card,void *dst,size_t block,size_t count,size_t len){
    (void)card;(void)block;assert(count==1);transfers++;transferred=dst;transferred_length=len;
    int ret=esp_cache_msync(dst,len,ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    return ret?ret:esp_cache_msync(dst,len,ESP_CACHE_MSYNC_FLAG_DIR_M2C);
}
static int sdmmc_write_sectors_dma(sdmmc_card_t *card,const void *src,size_t block,size_t count,size_t len){
    (void)card;(void)block;assert(count==1);transfers++;transferred=(void*)src;transferred_length=len;
    return esp_cache_msync((void*)src,len,ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}
SDK_ALLOCATE
SDK_CHUNK
SDK_READ
SDK_WRITE
int main(void){
    /* Real SDK's alignment accepts non-DMA ordinary P4 heap regions. */
    const uintptr_t addresses[]={0x30100000,0x50108000,0x4ff00000,0x48000000,0x4ffbff00,0x4bffff00};
    assert(sdmmc_host_check_buffer_alignment(0,(void*)0x30100000,512));
    assert(sdmmc_host_check_buffer_alignment(0,(void*)0x50108000,512));
    for(unsigned i=0;i<2;i++){
        for(int write=0;write<2;write++){
            sdmmc_card_t original={.csd={.sector_size=512},.host={.slot=0,.check_buffer_alignment=sdmmc_host_check_buffer_alignment}};
            transfers=allocations=0;void *ptr=(void*)addresses[i];
            int result=write?sdmmc_write_sectors(&original,ptr,0,1):sdmmc_read_sectors(&original,ptr,0,1);
            assert(result==ESP_ERR_NOT_SUPPORTED && allocations==0 && transfers==1 && transferred==ptr);
        }
    }
    for(unsigned i=0;i<sizeof(addresses)/sizeof(addresses[0]);i++){
        void *ptr=(void*)addresses[i];bool direct=i==2||(i==3&&SOC_SDMMC_PSRAM_DMA_CAPABLE);
        assert(sd_dma_buffer_alignment(0,ptr,512)==direct);
        for(int write=0;write<2;write++){
            transfers=allocations=copies=0;
            sdmmc_card_t card={.csd={.sector_size=512},.host={.slot=0,.check_buffer_alignment=sd_dma_buffer_alignment}};
            int result=write?sdmmc_write_sectors(&card,ptr,0,1):sdmmc_read_sectors(&card,ptr,0,1);
            assert(result==0&&transfers==1&&transferred_length==512);
            assert(transferred==(direct?ptr:(void*)0x4ff00000));
            assert(allocations==!direct&&copies==!direct);
        }
    }
    assert(!sd_dma_buffer_alignment(0,NULL,512));
    assert(!sd_dma_buffer_alignment(0,(void*)0x4ff00000,0));
    assert(!sd_dma_buffer_alignment(0,(void*)(UINTPTR_MAX-63),512));
    assert(!sd_dma_buffer_alignment(0,(void*)0x4ff00001,512));
    assert(!sd_dma_buffer_alignment(0,(void*)0x4ff00000,511));
    /* Do not alter C6's original SDK dispatch contract. */
    assert(sd_dma_buffer_alignment(1,(void*)0x30100000,512));
    sdmmc_card_t card={.csd={.sector_size=512},.host={.slot=0,.check_buffer_alignment=sd_dma_buffer_alignment}};
    fail_allocate=true;transfers=0;
    assert(sdmmc_read_sectors(&card,(void*)0x30100000,0,1)==ESP_ERR_NO_MEM&&transfers==0);
    assert(sdmmc_write_sectors(&card,(void*)0x30100000,0,1)==ESP_ERR_NO_MEM&&transfers==0);
    puts("PASS actual SD sector read/write dispatch: non-DMA P4 heap uses DMA bounce; no ignored cache error");
    return 0;
}
'''


def source():
    command=(SDK/"components/sdmmc/sdmmc_cmd.c").read_text()
    host=(SDK/"components/esp_driver_sdmmc/src/sd_host_sdmmc.c").read_text()
    cache=(SDK/"components/hal/esp32p4/include/hal/cache_ll.h").read_text()
    msync=(SDK/"components/esp_mm/esp_cache_msync.c").read_text()
    pointer=(SDK/"components/esp_hw_support/include/esp_memory_utils.h").read_text()
    project=(ROOT/"main/pk_sdcard.c").read_text()
    signature="static bool sd_dma_buffer_alignment("
    wrapper=function(project,signature) if signature in project else (
        "static bool sd_dma_buffer_alignment(int slot,const void *buf,size_t size){return sdmmc_host_check_buffer_alignment(slot,buf,size);}\n")
    replacements={"SDK_ALIGNMENT":function(host,"bool sd_host_check_buffer_alignment("),
                  "PROJECT_ALIGNMENT":wrapper,
                  "DMA_POINTER":function(pointer,"inline static bool esp_ptr_dma_capable("),
                  "CACHE_MAP":function(cache,"static inline bool cache_ll_vaddr_to_cache_level_id("),
                  "CACHE_SYNC":function(msync,"esp_err_t esp_cache_msync("),
                  "SDK_ALLOCATE":function(command,"static esp_err_t allocate_dma_buf("),
                  "SDK_CHUNK":function(command,"static inline size_t get_chunk_size("),
                  "SDK_READ":function(command,"esp_err_t sdmmc_read_sectors("),
                  "SDK_WRITE":function(command,"esp_err_t sdmmc_write_sectors(")}
    result=HARNESS
    for old,new in replacements.items():result=result.replace(old,new)
    return result


def run(source, psram):
    with tempfile.TemporaryDirectory(prefix="pk-sd-dma-range-") as directory:
        p=Path(directory);c=p/"test.c";exe=p/"test"
        c.write_text(source)
        # The unchanged SDK cache function explicitly casts 32-bit target
        # addresses; this fixture supplies those addresses on a 64-bit host.
        compiled=subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-Wno-unused-function","-Wno-void-pointer-to-int-cast",
                                 f"-DSOC_SDMMC_PSRAM_DMA_CAPABLE={psram}",str(c),"-o",str(exe)],text=True,capture_output=True)
        if compiled.returncode:raise AssertionError(compiled.stderr)
        return subprocess.run([str(exe)],text=True,capture_output=True)


class SdDmaBufferTest(unittest.TestCase):
    def test_real_sdk_alignment_and_sector_branches(self):
        for psram in (0,1):
            with self.subTest(psram=psram):
                result=run(source(),psram)
                self.assertEqual(result.returncode,0,result.stderr)

    def test_range_and_sector_dispatch_mutations_are_rejected(self):
        original=source()
        mutations=(
            ("return dma_range && sdmmc_host_check_buffer_alignment(slot, buf, size);",
             "(void)dma_range; return sdmmc_host_check_buffer_alignment(slot, buf, size);"),
            ("esp_ptr_dma_capable(buf) && esp_ptr_dma_capable(last)",
             "esp_ptr_dma_capable(buf) && ((void)last, true)"),
            ("esp_ptr_dma_ext_capable(buf) && esp_ptr_dma_ext_capable(last)",
             "esp_ptr_dma_ext_capable(buf)"),
        )
        for index,(old,new) in enumerate(mutations):
            self.assertEqual(original.count(old),1)
            for psram in ((0,1) if index<2 else (1,)):
                with self.subTest(mutation=index,psram=psram):
                    result=run(original.replace(old,new,1),psram)
                    self.assertNotEqual(result.returncode,0,"DMA-domain mutation survived")


if __name__=="__main__":unittest.main()
