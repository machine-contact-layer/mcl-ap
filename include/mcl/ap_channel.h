#ifndef MCL_AP_CHANNEL_H
#define MCL_AP_CHANNEL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCL_AP_REFERENCE_TEMPERATURE_K 293.15
#define MCL_AP_REFERENCE_PRESSURE_KPA 101.325
#define MCL_AP_TRIPLE_TEMPERATURE_K 273.16

/* Approximate dry-air sound speed near ordinary atmospheric conditions: c = 331.3 + 0.606*T [m/s] */
double mcl_ap_speed_of_sound_mps(double temperature_c);

/* ISO standard sound speed: c = 343.2 * sqrt(T / T0) [m/s] */
double mcl_ap_iso_sound_speed_mps(double temperature_c);

/* One-way acoustic propagation delay in seconds */
double mcl_ap_one_way_delay_s(double distance_m, double temperature_c);

/* Propagation-only round-trip delay in seconds */
double mcl_ap_round_trip_delay_s(double distance_m, double temperature_c);

/* Free-field spherical spreading loss in dB: 20 * log10(r / r0) */
double mcl_ap_spherical_spreading_db(double distance_m, double reference_m);

/* First-order acoustic Doppler shift in Hz: delta_f = f * v / c */
double mcl_ap_doppler_shift_hz(double carrier_hz, double relative_radial_velocity_mps, double temperature_c);

/* Payload serialization time in seconds at net bitrate */
double mcl_ap_serialization_time_s(size_t payload_bytes, double net_bitrate_bps);

/* Bitrate in bps where serialization time equals one-way propagation time */
double mcl_ap_bitrate_equal_to_propagation_bps(size_t payload_bytes, double distance_m, double temperature_c);

/* ISO 9613-1 saturation vapor pressure in kPa */
double mcl_ap_saturation_pressure_kpa(double temperature_k);

/* ISO 9613-1 atmospheric attenuation in dB/m */
double mcl_ap_attenuation_db_per_m(
    double frequency_hz,
    double temperature_c,
    double relative_humidity_percent,
    double pressure_kpa);

/* Free-field loss (spherical spreading + atmospheric absorption) in dB */
double mcl_ap_free_field_loss_db(
    double distance_m,
    double frequency_hz,
    double reference_distance_m,
    double temperature_c,
    double relative_humidity_percent,
    double pressure_kpa);

#ifdef __cplusplus
}
#endif

#endif /* MCL_AP_CHANNEL_H */
