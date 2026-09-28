#pragma once
#include <stdlib.h>
#include <string.h>
typedef struct { char data[8192];size_t n; } *StreamBufferHandle_t;
static inline StreamBufferHandle_t xStreamBufferCreate(size_t n,size_t trigger)
{ (void)n;(void)trigger;return calloc(1,sizeof(*((StreamBufferHandle_t)0))); }
static inline size_t xStreamBufferSend(StreamBufferHandle_t b,const void *p,size_t n,unsigned ticks)
{ (void)ticks;if(n>sizeof(b->data)-b->n)n=sizeof(b->data)-b->n;memcpy(b->data+b->n,p,n);b->n+=n;return n; }
static inline size_t xStreamBufferReceive(StreamBufferHandle_t b,void *p,size_t n,unsigned ticks)
{ (void)ticks;if(n>b->n)n=b->n;memcpy(p,b->data,n);b->n-=n;memmove(b->data,b->data+n,b->n);return n; }
static inline size_t xStreamBufferBytesAvailable(StreamBufferHandle_t b) { return b->n; }
