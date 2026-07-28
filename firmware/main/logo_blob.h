/*
 * logo_blob.h — 取出内置的 Pilot Kit logo 位图。
 *
 * 同一份 160×160 RGB565 数据，开机画面（boot_splash.c）与「关于」页共用。
 * 固件里它由 CMakeLists 的 EMBED_FILES 链进 .rodata；模拟器没有那套机制，
 * 由 sim/compat/page_stub.c 从磁盘读同一个文件。调用方两边一致。
 */
#pragma once

#include <stdint.h>

/* 返回像素数据（RGB565）。取不到时返回 NULL。
 *
 * 名字不叫 pk_logo_rgb565：ESP-IDF 的 EMBED_FILES 会按文件名
 * （pk_logo.rgb565）自动生成一个同名符号，撞名会在链接期报 multiple
 * definition。
 * w/h 非空时写入源图尺寸。 */
const uint16_t *pk_logo_bitmap(int *w, int *h);
