/* pk_vib.c — see pk_vib.h for the algorithm and the 0="unavailable"
 * vs. small-nonzero="calm" contract. */
#include "pk_vib.h"

#include <math.h>
#include <string.h>

void pk_vib_reset(pk_vib_state_t *st)
{
    memset(st, 0, sizeof(*st));
}

void pk_vib_push(pk_vib_state_t *st, float ax, float ay, float az)
{
    float sq = ax * ax + ay * ay + az * az;

    uint16_t idx = st->head;
    if (st->count >= PK_VIB_WINDOW_LEN) {
        /* Window already full: evict the sample this write clobbers. */
        st->sum_sq -= st->buf[idx];
    } else {
        st->count++;
    }
    st->buf[idx] = sq;
    st->sum_sq += sq;

    st->head = (uint16_t)((idx + 1) % PK_VIB_WINDOW_LEN);
}

uint8_t pk_vib_level(const pk_vib_state_t *st)
{
    if (st->count < PK_VIB_WINDOW_LEN) {
        return 0;   /* window not full yet — "unavailable", not "calm" */
    }

    float mean = st->sum_sq / (float)PK_VIB_WINDOW_LEN;
    if (mean < 0.0f) {
        mean = 0.0f;   /* guard against fp cancellation noise from the
                           running sum going fractionally negative */
    }
    float rms = sqrtf(mean);

    float scaled = rms * (255.0f / PK_VIB_RMS_FULLSCALE_MPS2);
    int32_t q = (int32_t)(scaled + 0.5f);
    if (q < 1)   q = 1;     /* floor: window-full state must never read
                               as 0 ("unavailable"), even at true rest */
    if (q > 255) q = 255;
    return (uint8_t)q;
}
