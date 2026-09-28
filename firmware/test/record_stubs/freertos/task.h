#pragma once
#include "FreeRTOS.h"
typedef unsigned TickType_t;
static int pk_host_task_create_fail;
static void (*pk_host_created_task)(void *);
static void (*pk_host_task_create_hook)(void);
static void (*pk_host_delay_hook)(unsigned);
static void (*pk_host_delete_hook)(void *);
static inline int xTaskCreatePinnedToCore(void (*fn)(void *),const char *name,
    unsigned stack,void *arg,unsigned priority,void *handle,int core)
{
    (void)name;(void)stack;(void)arg;(void)priority;(void)handle;(void)core;
    if(pk_host_task_create_fail)return pdFALSE;
    pk_host_created_task=fn;if(pk_host_task_create_hook)pk_host_task_create_hook();return pdTRUE;
}
static inline int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,
    void *arg,unsigned priority,void *handle)
{ return xTaskCreatePinnedToCore(fn,name,stack,arg,priority,handle,-1); }
static inline void vTaskDelay(TickType_t ticks) { if(pk_host_delay_hook)pk_host_delay_hook(ticks); }
static inline void vTaskDelete(void *handle) { if(pk_host_delete_hook)pk_host_delete_hook(handle); }
