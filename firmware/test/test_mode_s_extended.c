/*
 * test_mode_s_extended.c — Mode-S altitude / velocity / geometric-position
 * decoder regression vectors.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -o /tmp/test_mode_s_extended firmware/test/test_mode_s_extended.c -lm \
 *   && /tmp/test_mode_s_extended
 *
 * The hex vectors are independent, captured ADS-B examples.  Expected values
 * are hand-derived from DO-260B fields, not from mode_s.c helpers:
 *   - velocity encoded magnitude 0=N/A, otherwise (raw-1)*scale;
 *   - subtype 1/3 scale=1 kt, subtype 2/4 scale=4 kt;
 *   - heading is clockwise atan2(E/W, N/S), or raw*360/1024;
 *   - vertical rate is (raw-1)*64 fpm with an independent sign bit;
 *   - TC20-22 altitude is GNSS HAE in metres.
 */
#include "mode_s.c"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)
#define CHECK_EQ_I(got, want, label) \
    CHECK((got) == (want), "%s got=%d want=%d\n", label, (int)(got), (int)(want))
#define CHECK_NEAR(got, want, tol, label) \
    CHECK(fabs((double)(got) - (double)(want)) <= (tol), \
          "%s got=%.6f want=%.6f\n", label, (double)(got), (double)(want))

static void from_hex(const char *hex, unsigned char msg[14])
{
    for (int i = 0; i < 14; ++i) {
        unsigned int value = 0;
        (void)sscanf(hex + i * 2, "%2x", &value);
        msg[i] = (unsigned char)value;
    }
}

static void decode_hex(const char *hex, struct mode_s_msg *mm)
{
    unsigned char msg[14];
    mode_s_t dec;
    from_hex(hex, msg);
    mode_s_init(&dec);
    dec.fix_errors = 0;
    memset(mm, 0xA5, sizeof(*mm));
    mode_s_decode(&dec, mm, msg);
}

static void set_airborne_altitude(unsigned char msg[14], int type_code,
                                  unsigned int ac12)
{
    memset(msg, 0, 14);
    msg[0] = (unsigned char)(17 << 3);
    msg[1] = 0xAB; msg[2] = 0xCD; msg[3] = 0xEF;
    msg[4] = (unsigned char)(type_code << 3);
    msg[5] = (unsigned char)(ac12 >> 4);
    msg[6] = (unsigned char)((ac12 & 0x0F) << 4);
}

static void test_velocity_vectors(void)
{
    struct mode_s_msg mm;

    decode_hex("8D485020994409940838175B284F", &mm);
    CHECK_EQ_I(mm.msgtype, 17, "subtype1 DF");
    CHECK_EQ_I(mm.metype, 19, "subtype1 TC");
    CHECK_EQ_I(mm.mesub, 1, "subtype1 subtype");
    CHECK_EQ_I(mm.ew_velocity, 8, "subtype1 E/W magnitude-1");
    CHECK_EQ_I(mm.ns_velocity, 159, "subtype1 N/S magnitude-1");
    CHECK_EQ_I(mm.velocity, 159, "subtype1 ground speed");
    CHECK_NEAR(mm.heading, 182.8803776, 0.01, "subtype1 track");
    CHECK_EQ_I(mm.vert_rate, 14, "subtype1 vertical-rate encoded magnitude");

    decode_hex("8DA05F219B06B6AF189400CBC33F", &mm);
    CHECK_EQ_I(mm.mesub, 3, "subtype3 subtype");
    CHECK_NEAR(mm.heading, 243.984375, 0.01, "subtype3 10-bit heading");
    CHECK_EQ_I(mm.velocity, 375, "subtype3 airspeed magnitude-1");
    CHECK_EQ_I(mm.vert_rate, 37, "subtype3 vertical-rate encoded magnitude");
    CHECK_EQ_I((mm.vert_rate - 1) * 64 * (mm.vert_rate_sign ? -1 : 1),
               -2304, "subtype3 vertical rate fpm");

    /* Same payload magnitudes, supersonic subtype: each non-zero encoded
     * step is four knots. */
    unsigned char msg[14];
    mode_s_t dec;
    from_hex("8D485020994409940838175B284F", msg);
    msg[4] = (unsigned char)((msg[4] & 0xF8) | 2);
    mode_s_init(&dec);
    mode_s_decode(&dec, &mm, msg);
    CHECK_EQ_I(mm.ew_velocity, 32, "subtype2 E/W x4");
    CHECK_EQ_I(mm.ns_velocity, 636, "subtype2 N/S x4");
    CHECK_EQ_I(mm.velocity, 636, "subtype2 speed x4");

    from_hex("8DA05F219B06B6AF189400CBC33F", msg);
    msg[4] = (unsigned char)((msg[4] & 0xF8) | 4);
    mode_s_decode(&dec, &mm, msg);
    CHECK_EQ_I(mm.velocity, 1500, "subtype4 airspeed x4");

    /* Zero magnitude is N/A, not zero knots; poison the destination to prove
     * mode_s_decode clears and explicitly invalidates every absent field. */
    from_hex("8D485020994409940838175B284F", msg);
    msg[5] &= 0xFC; msg[6] = 0; /* E/W magnitude = 0 */
    msg[8] &= 0xF8; msg[9] &= 0x03; /* vertical-rate magnitude = 0 */
    memset(&mm, 0xA5, sizeof(mm));
    mode_s_decode(&dec, &mm, msg);
    CHECK(!mm.velocity_valid, "one unavailable vector component must invalidate speed\n");
    CHECK(!mm.heading_is_valid, "one unavailable component must invalidate track\n");
    CHECK(!mm.vert_rate_valid, "zero vertical-rate code must mean unavailable\n");
    CHECK_EQ_I(mm.velocity, 0, "unavailable speed is cleared");
}

static void test_gillham_and_zero_altitude(void)
{
    mode_s_t dec;
    struct mode_s_msg mm;
    unsigned char msg[14];
    mode_s_init(&dec);

    /* Gillham D1D2D4 A1A2A4 B1B2B4 C1C2C4 = 000111001011
     * is 21,900 ft.  Reordered into AC12 it is 0x7C2. */
    set_airborne_altitude(msg, 9, 0x7C2);
    memset(&mm, 0, sizeof(mm));
    mode_s_decode(&dec, &mm, msg);
    CHECK_EQ_I(mm.altitude, 21900, "AC12 Q=0 Gillham altitude");
    CHECK(mm.altitude_valid, "AC12 Q=0 Gillham must be valid\n");
    CHECK_EQ_I(mm.altitude_source, MODE_S_ALTITUDE_BARO,
               "AC12 Gillham altitude source");

    /* Captured Q=1 vector guards the ordinary 25-ft path too. */
    decode_hex("8D896113487361C6EA41C40703ED", &mm);
    CHECK_EQ_I(mm.altitude, 21950, "AC12 Q=1 captured altitude");

    /* Gillham 000000011010 is a valid 0 ft code.  Its value must not be
     * conflated with an unavailable all-zero AC12 field. */
    set_airborne_altitude(msg, 9, 0x20A);
    memset(&mm, 0, sizeof(mm));
    mode_s_decode(&dec, &mm, msg);
    CHECK_EQ_I(mm.altitude, 0, "AC12 valid zero-foot altitude");
    CHECK(mm.altitude_valid, "AC12 zero-foot code must remain valid\n");

    set_airborne_altitude(msg, 9, 0);
    memset(&mm, 0, sizeof(mm));
    mode_s_decode(&dec, &mm, msg);
    CHECK(!mm.altitude_valid, "all-zero AC12 must be unavailable\n");

    /* AC13 carries the same Gillham bits with an explicit M position.
     * 0xF82 is the 21,900-ft code used above before removal of M. */
    memset(msg, 0, sizeof(msg));
    msg[0] = (unsigned char)(20 << 3);
    msg[2] = 0x0F; msg[3] = 0x82;
    memset(&mm, 0, sizeof(mm));
    mode_s_decode(&dec, &mm, msg);
    CHECK(mm.altitude_valid, "AC13 Q=0 Gillham must be valid\n");
    CHECK_EQ_I(mm.altitude, 21900, "AC13 Q=0 Gillham altitude");
}

static void test_tc20_gnss_altitude_and_cpr(void)
{
    struct mode_s_msg mm;
    decode_hex("8E7C6296A0A0A59C64468D6F4EDD", &mm);
    CHECK_EQ_I(mm.metype, 20, "TC20 type code");
    CHECK_EQ_I(mm.altitude, 2570, "TC20 raw GNSS altitude metres");
    CHECK_EQ_I(mm.unit, MODE_S_UNIT_METERS, "TC20 GNSS altitude unit");
    CHECK(mm.altitude_valid, "TC20 GNSS altitude must be valid\n");
    CHECK_EQ_I(mm.altitude_source, MODE_S_ALTITUDE_GNSS,
               "TC20 GNSS altitude source");
    CHECK_EQ_I(mm.raw_latitude, 52786, "TC20 CPR latitude");
    CHECK_EQ_I(mm.raw_longitude, 18061, "TC20 CPR longitude");
    CHECK_EQ_I((int)(mm.altitude * 3.28084), 8431, "TC20 GNSS altitude feet");
}

static void test_long_df_length_rule(void)
{
    CHECK_EQ_I(mode_s_msg_len_by_type(18), 112, "DF18 length");
    CHECK_EQ_I(mode_s_msg_len_by_type(24), 112, "DF24 Comm-D length");
}

int main(void)
{
    test_velocity_vectors();
    test_gillham_and_zero_altitude();
    test_tc20_gnss_altitude_and_cpr();
    test_long_df_length_rule();
    printf(g_fail ? "FAIL (%d)\n" : "PASS: extended Mode-S vectors\n", g_fail);
    return g_fail ? 1 : 0;
}
