#include "airband_ble_codec.h"
#include <limits.h>
#include <string.h>

static const int s_step[89] = {
 7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
 73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,
 408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,
 1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
 5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,
 18500,20350,22385,24623,27086,29794,32767
};
static const int8_t s_index_delta[16] = {
 -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8
};
static void wr16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void wr32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}

static uint8_t encode_sample(pk_airband_ima_state_t *s, int16_t sample)
{
    int index=s->step_index; if(index>88) index=88;
    int step=s_step[index], diff=(int)sample-(int)s->predictor;
    uint8_t code=0; if(diff<0){code=8;diff=-diff;}
    int delta=step>>3;
    if(diff>=step){code|=4;diff-=step;delta+=step;}
    if(diff>=(step>>1)){code|=2;diff-=step>>1;delta+=step>>1;}
    if(diff>=(step>>2)){code|=1;delta+=step>>2;}
    int predictor=(int)s->predictor+((code&8)?-delta:delta);
    if(predictor>INT16_MAX) predictor=INT16_MAX;
    if(predictor<INT16_MIN) predictor=INT16_MIN;
    index += s_index_delta[code];
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    s->predictor=(int16_t)predictor;s->step_index=(uint8_t)index;
    return code;
}

size_t pk_airband_ble_encode_frame(pk_airband_ima_state_t *state,
                                   uint16_t stream_id,uint16_t frame_seq,
                                   uint32_t sample_clock,
                                   const int16_t pcm[PK_AIRBAND_BLE_FRAME_SAMPLES],
                                   uint8_t *out,size_t capacity)
{
    if(!state||!pcm||!out||capacity<PK_AIRBAND_BLE_FRAME_BYTES||state->step_index>88)return 0;
    out[0]=PK_AIRBAND_BLE_PROTOCOL_VERSION;out[1]=0;
    wr16(out+2,stream_id);wr16(out+4,frame_seq);wr16(out+6,PK_AIRBAND_BLE_FRAME_SAMPLES);
    wr32(out+8,sample_clock);wr16(out+12,(uint16_t)state->predictor);
    out[14]=state->step_index;out[15]=0;
    for(size_t i=0;i<PK_AIRBAND_BLE_FRAME_SAMPLES;i+=2){
        uint8_t lo=encode_sample(state,pcm[i]);
        uint8_t hi=encode_sample(state,pcm[i+1]);
        out[PK_AIRBAND_BLE_FRAME_HEADER_BYTES+i/2]=(uint8_t)(lo|(hi<<4));
    }
    return PK_AIRBAND_BLE_FRAME_BYTES;
}

size_t pk_airband_ble_fragment_count(size_t len,uint16_t mtu)
{
    if(!len||mtu<=3)return 0;
    size_t payload=mtu-3u;
    if(len<=payload)return 1;
    if(payload<=PK_AIRBAND_BLE_FRAGMENT_HEADER_BYTES)return 0;
    size_t data=payload-PK_AIRBAND_BLE_FRAGMENT_HEADER_BYTES;
    if(data<PK_AIRBAND_BLE_MIN_FRAGMENT_DATA)return 0;
    size_t count=(len+data-1u)/data;
    return count<=255u?count:0;
}

size_t pk_airband_ble_make_fragment(uint16_t stream,uint16_t seq,
                                    const uint8_t *frame,size_t len,uint16_t mtu,
                                    size_t index,uint8_t *out,size_t capacity)
{
    size_t count=pk_airband_ble_fragment_count(len,mtu);
    if(!frame||!out||count<=1||index>=count||len>UINT16_MAX)return 0;
    size_t payload=mtu-3u, data=payload-PK_AIRBAND_BLE_FRAGMENT_HEADER_BYTES;
    size_t off=index*data,take=len-off;if(take>data)take=data;
    size_t n=PK_AIRBAND_BLE_FRAGMENT_HEADER_BYTES+take;if(capacity<n)return 0;
    out[0]=0xaf;out[1]=PK_AIRBAND_BLE_PROTOCOL_VERSION;wr16(out+2,stream);wr16(out+4,seq);
    out[6]=(uint8_t)index;out[7]=(uint8_t)count;wr16(out+8,(uint16_t)off);wr16(out+10,(uint16_t)len);
    memcpy(out+PK_AIRBAND_BLE_FRAGMENT_HEADER_BYTES,frame+off,take);return n;
}
