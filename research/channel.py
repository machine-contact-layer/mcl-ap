"""Foundational deterministic calculations for MCL-AP research.

These functions are intentionally small and dependency-free. They are not a complete
acoustic channel simulator; they provide sanity-check physics used by experiments.
"""

from __future__ import annotations

import math


def speed_of_sound_mps(temperature_c: float = 20.0) -> float:
    """Approximate dry-air sound speed near ordinary atmospheric conditions.

    c ≈ 331.3 + 0.606*T [m/s]
    """
    return 331.3 + 0.606 * temperature_c


def one_way_delay_s(distance_m: float, temperature_c: float = 20.0) -> float:
    """One-way acoustic propagation delay."""
    if distance_m < 0:
        raise ValueError("distance_m must be non-negative")
    return distance_m / speed_of_sound_mps(temperature_c)


def round_trip_delay_s(distance_m: float, temperature_c: float = 20.0) -> float:
    """Propagation-only request/reply RTT floor for equal path length."""
    return 2.0 * one_way_delay_s(distance_m, temperature_c)


def spherical_spreading_db(distance_m: float, reference_m: float = 1.0) -> float:
    """Free-field pressure loss from spherical spreading.

    Returns 20 log10(r/r0). This excludes absorption, ground, obstruction,
    multipath, directivity and device response.
    """
    if distance_m <= 0 or reference_m <= 0:
        raise ValueError("distance and reference must be positive")
    return 20.0 * math.log10(distance_m / reference_m)


def doppler_shift_hz(
    carrier_hz: float,
    relative_radial_velocity_mps: float,
    temperature_c: float = 20.0,
) -> float:
    """First-order acoustic Doppler shift approximation Δf ≈ f*v/c.

    Positive relative velocity means closing speed for magnitude purposes.
    Use a more exact source/receiver model when direction-specific precision is
    required.
    """
    c = speed_of_sound_mps(temperature_c)
    return carrier_hz * relative_radial_velocity_mps / c


def serialization_time_s(payload_bytes: int, net_bitrate_bps: float) -> float:
    """Time required to serialize a payload at a net bitrate."""
    if payload_bytes < 0:
        raise ValueError("payload_bytes must be non-negative")
    if net_bitrate_bps <= 0:
        raise ValueError("net_bitrate_bps must be positive")
    return payload_bytes * 8.0 / net_bitrate_bps


def bitrate_equal_to_propagation_bps(
    payload_bytes: int,
    distance_m: float,
    temperature_c: float = 20.0,
) -> float:
    """Bitrate where payload serialization time equals one-way propagation time."""
    delay = one_way_delay_s(distance_m, temperature_c)
    if delay == 0:
        return math.inf
    return payload_bytes * 8.0 / delay


if __name__ == "__main__":
    for distance in (1, 5, 10, 20, 30, 50, 100, 300):
        delay_ms = one_way_delay_s(distance) * 1_000
        eq_kbps = bitrate_equal_to_propagation_bps(32, distance) / 1_000
        print(f"{distance:>3} m  delay={delay_ms:8.2f} ms  32B-equal={eq_kbps:8.2f} kb/s")
