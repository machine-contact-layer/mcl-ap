#include "mcl/ap_channel.h"
#include <math.h>
#include <stddef.h>

double mcl_ap_speed_of_sound_mps(double temperature_c)
{
    return 331.3 + 0.606 * temperature_c;
}

double mcl_ap_iso_sound_speed_mps(double temperature_c)
{
    const double T = temperature_c + 273.15;
    return 343.2 * sqrt(T / MCL_AP_REFERENCE_TEMPERATURE_K);
}

double mcl_ap_one_way_delay_s(double distance_m, double temperature_c)
{
    if (distance_m < 0.0) {
        return 0.0;
    }
    return distance_m / mcl_ap_speed_of_sound_mps(temperature_c);
}

double mcl_ap_round_trip_delay_s(double distance_m, double temperature_c)
{
    return 2.0 * mcl_ap_one_way_delay_s(distance_m, temperature_c);
}

double mcl_ap_spherical_spreading_db(double distance_m, double reference_m)
{
    if (distance_m <= 0.0 || reference_m <= 0.0) {
        return 0.0;
    }
    return 20.0 * log10(distance_m / reference_m);
}

double mcl_ap_doppler_shift_hz(double carrier_hz, double relative_radial_velocity_mps, double temperature_c)
{
    const double c = mcl_ap_speed_of_sound_mps(temperature_c);
    return carrier_hz * relative_radial_velocity_mps / c;
}

double mcl_ap_serialization_time_s(size_t payload_bytes, double net_bitrate_bps)
{
    if (net_bitrate_bps <= 0.0) {
        return 0.0;
    }
    return ((double)payload_bytes * 8.0) / net_bitrate_bps;
}

double mcl_ap_bitrate_equal_to_propagation_bps(size_t payload_bytes, double distance_m, double temperature_c)
{
    const double delay = mcl_ap_one_way_delay_s(distance_m, temperature_c);
    if (delay <= 0.0) {
        return 0.0;
    }
    return ((double)payload_bytes * 8.0) / delay;
}

double mcl_ap_saturation_pressure_kpa(double temperature_k)
{
    if (temperature_k <= 0.0) {
        return 0.0;
    }
    return MCL_AP_REFERENCE_PRESSURE_KPA * pow(10.0, -6.8346 * pow(MCL_AP_TRIPLE_TEMPERATURE_K / temperature_k, 1.261) + 4.6151);
}

double mcl_ap_attenuation_db_per_m(
    double frequency_hz,
    double temperature_c,
    double relative_humidity_percent,
    double pressure_kpa)
{
    const double f = frequency_hz;
    const double T = temperature_c + 273.15;
    const double T0 = MCL_AP_REFERENCE_TEMPERATURE_K;
    const double p0 = MCL_AP_REFERENCE_PRESSURE_KPA;
    const double p = pressure_kpa > 0.0 ? pressure_kpa : p0;
    const double psat = mcl_ap_saturation_pressure_kpa(T);
    const double h = relative_humidity_percent * psat / p;
    const double fr_o = (p / p0) * (24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h));
    const double fr_n = (p / p0) * pow(T / T0, -0.5) * (9.0 + 280.0 * h * exp(-4.170 * (pow(T / T0, -1.0 / 3.0) - 1.0)));
    const double term1 = 1.84e-11 * (p0 / p) * pow(T / T0, 0.5);
    const double term2 = pow(T / T0, -2.5) * (0.01275 * exp(-2239.1 / T) / (fr_o + (f * f) / fr_o) +
                                             0.1068 * exp(-3352.0 / T) / (fr_n + (f * f) / fr_n));

    return 8.686 * (f * f) * (term1 + term2);
}

double mcl_ap_free_field_loss_db(
    double distance_m,
    double frequency_hz,
    double reference_distance_m,
    double temperature_c,
    double relative_humidity_percent,
    double pressure_kpa)
{
    double spreading;
    double alpha;
    double atmosphere;

    if (distance_m < reference_distance_m || reference_distance_m <= 0.0) {
        return 0.0;
    }

    spreading = 20.0 * log10(distance_m / reference_distance_m);
    alpha = mcl_ap_attenuation_db_per_m(frequency_hz, temperature_c, relative_humidity_percent, pressure_kpa);
    atmosphere = alpha * (distance_m - reference_distance_m);

    return spreading + atmosphere;
}
