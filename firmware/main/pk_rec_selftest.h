/*
 * pk_rec_selftest.h — ADS-B / 本机数据落盘的注入式自检。
 *
 * 设计依据 ADS-B 数据持久化设计（内部文档）
 * 「验证」节：「注入式自检：新增调试入口，把预置 ts-line 样本喂进
 * record_dispatch，走完整写入链路后校验文件内容与索引。不依赖射频，
 * 接收机没插也能证明全链路通」。
 *
 * 编译期开关，默认关（照 apt_detail_page.h 的 PK_APT_DETAIL_SMOKE 先例
 * ——同一套"真机自检默认不编译，需要时改宏重新烧录"的风格）。打开后开机
 * 起一个自检任务，跑完把 PASS/FAIL 打进日志（ESP_LOGE 逐条列出失败的
 * 断言）。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define PK_REC_SELFTEST 0

/* 自检任务的创建入口。PK_REC_SELFTEST=0 时是个空实现。 */
void pk_rec_selftest_init(void);

#ifdef __cplusplus
}
#endif
