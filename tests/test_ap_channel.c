#include "mcl/ap_channel.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK_NEAR(val, expected, tolerance) do { \
    const double v__ = (val); \
    const double e__ = (expected); \
    const double tol__ = (tolerance); \
    if (fabs(v__ - e__) > tol__) { \
        fprintf(stderr, "FAIL at %s:%d: got %f, expected %f (tolerance %f)\n", \
                __FILE__, __LINE__, v__, e__, tol__); \
        exit(1); \
    } \
} while (0)

#define CHECK_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL at %s:%d: (%s) is false\n", \
                __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

static void test_propagation_delays_and_bitrate(void)
{
    const double distances[] = {1.0, 5.0, 10.0, 20.0, 30.0, 50.0, 100.0, 300.0};
    const size_t num_distances = sizeof(distances) / sizeof(distances[0]);
    size_t i;
    const double c = mcl_ap_speed_of_sound_mps(20.0);

    CHECK_NEAR(c, 343.42, 0.01);

    for (i = 0; i < num_distances; ++i) {
        const double d = distances[i];
        const double delay_s = mcl_ap_one_way_delay_s(d, 20.0);
        const double delay_ms = delay_s * 1000.0;
        const double rtt_s = mcl_ap_round_trip_delay_s(d, 20.0);
        const double eq_bps = mcl_ap_bitrate_equal_to_propagation_bps(32u, d, 20.0);
        const double eq_kbps = eq_bps / 1000.0;

        CHECK_NEAR(delay_s, d / 343.42, 1e-6);
        CHECK_NEAR(rtt_s, 2.0 * delay_s, 1e-6);
        CHECK_NEAR(eq_bps, (32.0 * 8.0) / delay_s, 1e-4);

        printf("%3.0f m  delay=%8.2f ms  32B-equal=%8.2f kb/s\n", d, delay_ms, eq_kbps);
    }
}

static void test_iso_atmospheric_absorption(void)
{
    /* Sanity values at 20 °C, 50% RH, 101.325 kPa in dB/km */
    const double a_1k = mcl_ap_attenuation_db_per_m(1000.0, 20.0, 50.0, 101.325) * 1000.0;
    const double a_2k = mcl_ap_attenuation_db_per_m(2000.0, 20.0, 50.0, 101.325) * 1000.0;
    const double a_4k = mcl_ap_attenuation_db_per_m(4000.0, 20.0, 50.0, 101.325) * 1000.0;
    const double a_8k = mcl_ap_attenuation_db_per_m(8000.0, 20.0, 50.0, 101.325) * 1000.0;

    printf("1 kHz  %6.2f dB/km (expected ~4.66 dB/km)\n", a_1k);
    printf("2 kHz  %6.2f dB/km (expected ~9.89 dB/km)\n", a_2k);
    printf("4 kHz  %6.2f dB/km (expected ~29.67 dB/km)\n", a_4k);
    printf("8 kHz  %6.2f dB/km (expected ~105.29 dB/km)\n", a_8k);

    CHECK_NEAR(a_1k, 4.66, 0.1);
    CHECK_NEAR(a_2k, 9.89, 0.1);
    CHECK_NEAR(a_4k, 29.67, 0.2);
    CHECK_NEAR(a_8k, 105.29, 0.5);

    /* Atmospheric loss between 1 m and 50 m */
    {
        const double loss_4k_50m = mcl_ap_attenuation_db_per_m(4000.0, 20.0, 50.0, 101.325) * (50.0 - 1.0);
        const double loss_20k_50m = mcl_ap_attenuation_db_per_m(20000.0, 20.0, 50.0, 101.325) * (50.0 - 1.0);

        printf("4 kHz atmospheric term (1m -> 50m):  %5.2f dB (expected ~1.45 dB)\n", loss_4k_50m);
        printf("20 kHz atmospheric term (1m -> 50m): %5.2f dB (expected ~25.7 dB)\n", loss_20k_50m);

        CHECK_NEAR(loss_4k_50m, 1.45, 0.1);
        CHECK_NEAR(loss_20k_50m, 25.7, 0.3);
    }
}

static void test_spreading_and_doppler(void)
{
    /* Spherical spreading */
    const double spread_10m = mcl_ap_spherical_spreading_db(10.0, 1.0);
    const double spread_100m = mcl_ap_spherical_spreading_db(100.0, 1.0);
    CHECK_NEAR(spread_10m, 20.0, 1e-4);
    CHECK_NEAR(spread_100m, 40.0, 1e-4);

    /* Doppler shift at 20 kHz with 10 m/s relative velocity */
    {
        const double doppler = mcl_ap_doppler_shift_hz(20000.0, 10.0, 20.0);
        const double expected = (20000.0 * 10.0) / 343.42;
        CHECK_NEAR(doppler, expected, 1e-3);
    }

    /* Serialization time */
    {
        const double t = mcl_ap_serialization_time_s(16u, 1000.0);
        CHECK_NEAR(t, 0.128, 1e-6);
    }
}

int main(void)
{
    test_propagation_delays_and_bitrate();
    test_iso_atmospheric_absorption();
    test_spreading_and_doppler();

    puts("mcl_ap channel analytical calculations: ALL PASS");
    return 0;
}
