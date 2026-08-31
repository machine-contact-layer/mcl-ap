"""ISO 9613-1 atmospheric absorption utilities for MCL-AP research.

The formula follows ISO 9613-1:1993 as implemented by python-acoustics.
Units:
- temperature_c: degrees Celsius
- pressure_kpa: kPa
- relative_humidity_percent: numerical percent, e.g. 50.0
- frequency_hz: Hz
- result: dB/m
"""
from __future__ import annotations
import math
import numpy as np

REFERENCE_TEMPERATURE_K = 293.15
REFERENCE_PRESSURE_KPA = 101.325
TRIPLE_TEMPERATURE_K = 273.16

def sound_speed_mps(temperature_c: float = 20.0) -> float:
    T = temperature_c + 273.15
    return 343.2 * math.sqrt(T / REFERENCE_TEMPERATURE_K)

def saturation_pressure_kpa(temperature_k: float) -> float:
    return REFERENCE_PRESSURE_KPA * 10.0 ** (-6.8346 * (TRIPLE_TEMPERATURE_K / temperature_k) ** 1.261 + 4.6151)

def attenuation_db_per_m(frequency_hz, temperature_c: float = 20.0, relative_humidity_percent: float = 50.0, pressure_kpa: float = REFERENCE_PRESSURE_KPA):
    f = np.asarray(frequency_hz, dtype=float)
    T = temperature_c + 273.15; T0 = REFERENCE_TEMPERATURE_K; p0 = REFERENCE_PRESSURE_KPA
    psat = saturation_pressure_kpa(T)
    # ISO/python-acoustics convention: numerical relative humidity in percent.
    h = relative_humidity_percent * psat / pressure_kpa
    fr_o = (pressure_kpa / p0) * (24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h))
    fr_n = (pressure_kpa / p0) * (T / T0) ** -0.5 * (9.0 + 280.0 * h * np.exp(-4.170 * ((T / T0) ** (-1.0 / 3.0) - 1.0)))
    return 8.686 * f**2 * (1.84e-11 * (p0 / pressure_kpa) * (T / T0) ** 0.5 + (T / T0) ** -2.5 * (0.01275 * np.exp(-2239.1 / T) / (fr_o + f**2 / fr_o) + 0.1068 * np.exp(-3352.0 / T) / (fr_n + f**2 / fr_n)))

def free_field_loss_db(distance_m: float, frequency_hz, *, reference_distance_m: float = 1.0, temperature_c: float = 20.0, relative_humidity_percent: float = 50.0, pressure_kpa: float = REFERENCE_PRESSURE_KPA):
    if distance_m < reference_distance_m: raise ValueError("distance must be >= reference distance")
    spreading = 20.0 * math.log10(distance_m / reference_distance_m)
    alpha = attenuation_db_per_m(frequency_hz, temperature_c, relative_humidity_percent, pressure_kpa)
    atmosphere = alpha * (distance_m - reference_distance_m)
    return spreading + atmosphere
