#ifndef __SENSORS__
#define __SENSORS__

#include <string.h>

#include "eeprom_float.h"
#include "config_map.h"
#include "fix16_math/fix16_math.h"
#include "median.h"
#include "app.h"

/*
  Sensors data source:

  - voltage: physical, immediate
  - current: physical, from shunt, immediate (but can have phase shift)
  - power: calculated
  - speed: calculated
*/
class Sensors
{
public:

  fix16_t speed = 0;
  fix16_t voltage = 0;
  fix16_t current = 0;
  fix16_t knob = 0; // Speed knob physical value, 0..1

  // Flags to simplify checks in other modules.
  // true on zero cross up/down, false in all other ticks
  bool zero_cross_up = false;
  bool zero_cross_down = false;

  // Config info
  fix16_t cfg_shunt_resistance_inv;
  fix16_t cfg_motor_resistance;
  fix16_t cfg_rpm_max_inv;
  fix16_t cfg_motor_inductance;
  fix16_t cfg_rekv_to_speed_factor;

  // Input from triac driver to reflect triac state. Needed for speed measure
  // to drop noise. Autoupdated by triac driver.
  bool in_triac_on = false;

  // Should be called with 40kHz frequency
  void tick()
  {
    // Do preliminary filtering of raw data + normalize result
    fetch_adc_data();

    if (prev_voltage == 0 && voltage > 0) zero_cross_up = true;
    else zero_cross_up = false;

    if (prev_voltage > 0 && voltage == 0) zero_cross_down = true;
    else zero_cross_down = false;

    // Poor man zero cross check (both up and down)
    if (zero_cross_up || zero_cross_down)
    {
      if (once_zero_crossed) once_period_counted = true;

      once_zero_crossed = true;

      // If full half-period was counted at least once, save number of
      // ticks in half-period
      if (once_period_counted) period_in_ticks = phase_counter;

      phase_counter = 0;
    }

    speed_tick();

    phase_counter++;
    prev_voltage = voltage;
    prev_current = current;
  }

  // Load config from emulated EEPROM
  void configure()
  {
    cfg_motor_resistance = fix16_from_float(
      eeprom_float_read(CFG_MOTOR_RESISTANCE_ADDR, CFG_MOTOR_RESISTANCE_DEFAULT)
    );

    cfg_rpm_max_inv = fix16_from_float(
      1.0F / eeprom_float_read(CFG_RPM_MAX_ADDR, CFG_RPM_MAX_DEFAULT)
    );

    // config shunt resistance - in mOhm (divide by 1000)
    // shunt amplifier gain - 50
    cfg_shunt_resistance_inv = fix16_from_float(1.0F /
      (eeprom_float_read(CFG_SHUNT_RESISTANCE_ADDR, CFG_SHUNT_RESISTANCE_DEFAULT)
        * 50
        / 1000)
    );

    cfg_motor_inductance = fix16_from_float(
      eeprom_float_read(CFG_MOTOR_INDUCTANCE_ADDR, CFG_MOTOR_INDUCTANCE_DEFAULT)
    );

    cfg_rekv_to_speed_factor = fix16_from_float(
      eeprom_float_read(CFG_REKV_TO_SPEED_FACTOR_ADDR, CFG_REKV_TO_SPEED_FACTOR_DEFAULT)
    );


  }

  // Split raw ADC data by separate buffers
  void adc_raw_data_load(uint16_t ADCBuffer[], uint32_t adc_data_offset)
  {
    for (int sample = 0; sample < ADC_FETCH_PER_TICK; sample++)
    {
      adc_voltage_temp_buf[sample] = ADCBuffer[adc_data_offset++];
      adc_current_temp_buf[sample] = ADCBuffer[adc_data_offset++];
      adc_knob_temp_buf[sample] = ADCBuffer[adc_data_offset++];
      adc_v_refin_temp_buf[sample] = ADCBuffer[adc_data_offset++];
    }
  }

private:
  // Temporary buffers for adc data, filled by "interrupt" (DMA)
  uint16_t adc_voltage_temp_buf[ADC_FETCH_PER_TICK];
  uint16_t adc_current_temp_buf[ADC_FETCH_PER_TICK];
  uint16_t adc_knob_temp_buf[ADC_FETCH_PER_TICK];
  uint16_t adc_v_refin_temp_buf[ADC_FETCH_PER_TICK];

  // 1. Calculate σ (discrete random variable)
  // 2. Drop everything with deviation > 2σ and count mean for the rest.
  //
  // https://upload.wikimedia.org/wikipedia/commons/8/8c/Standard_deviation_diagram.svg
  //
  // For efficiensy, don't use root square (work with σ^2 instead)
  //
  // !!! count sould NOT be > 16
  //
  // src - circular buffer
  // head - index of NEXT data to write
  // count - number of elements BACK from head to process
  // window - sigma multiplier (usually [1..2])
  //
  // Why this work? We use collision avoiding approach. Interrupt can happen,
  // but we work with tail, and data is written to head. If bufer is big enougth,
  // we have time to process tails until override.
  //
  uint32_t truncated_mean(uint16_t *src, int count, fix16_t window)
  {
    int idx = 0;

    // Count mean & sigma in one pass
    // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance
    idx = count;
    uint32_t s = 0;
    uint32_t s2 = 0;
    while (idx)
    {
      int val = src[--idx];
      s += val;
      s2 += val * val;
    }

    int mean = (s + (count >> 1)) / count;

    int sigma_square = (s2 - (s * s / count)) / (count - 1);
    // quick & dirty multiply to win^2, when win is in fix16 format.
    // we suppose win is 1..2, and sigma^2 - 24 bits max
    int sigma_win_square = ((((window >> 8) * (window >> 8)) >> 12) * sigma_square) >> 4;

    // Drop big deviations and count mean for the rest
    idx = count;
    int s_mean_filtered = 0;
    int s_mean_filtered_cnt = 0;

    while (idx)
    {
      int val = src[--idx];

      if ((mean - val) * (mean - val) < sigma_win_square)
      {
        s_mean_filtered += val;
        s_mean_filtered_cnt++;
      }
    }

    // Protection from zero div. Should never happen
    if (!s_mean_filtered_cnt) return mean;

    return (s_mean_filtered + (s_mean_filtered_cnt >> 1)) / s_mean_filtered_cnt;
  }

  void fetch_adc_data()
  {
    // Apply filters
    uint16_t adc_voltage = truncated_mean(adc_voltage_temp_buf, ADC_FETCH_PER_TICK, F16(1.1));
    uint16_t adc_current = truncated_mean(adc_current_temp_buf, ADC_FETCH_PER_TICK, F16(1.1));
    uint16_t adc_knob = truncated_mean(adc_knob_temp_buf, ADC_FETCH_PER_TICK, F16(1.1));
    uint16_t adc_v_refin =  truncated_mean(adc_v_refin_temp_buf, ADC_FETCH_PER_TICK, F16(1.1));

    // Now process the rest...

    // 4096 - maximum value of 12-bit integer
    // normalize to fix16_t[0.0..1.0]
    fix16_t knob_new = adc_knob << 4;

    // Use additional mean smoother for knob
    knob = (knob * 15 + knob_new) >> 4;


    // Vrefin - internal reference voltage, 1.2v
    // Vref - ADC reference voltage, equal to ADC supply voltage (~ 3.3v)
    // adc_vrefin = 1.2 / Vref * 4096
    fix16_t v_ref = fix16_div(F16(1.2), adc_v_refin << 4);

    // maximum ADC input voltage - Vref
    // current = adc_current_norm * v_ref / cfg_shunt_resistance
    current = fix16_mul(
      fix16_mul(adc_current << 4, cfg_shunt_resistance_inv),
      v_ref
    );

    // resistors in voltage divider - [ 2*150 kOhm, 1.5 kOhm ]
    // (divider ratio => 201)
    // voltage = adc_voltage * v_ref * (301.5 / 1.5);
    voltage = fix16_mul(fix16_mul(adc_voltage << 4, v_ref), F16(301.5/1.5));

  }

  // Holds number of tick when voltage crosses zero
  // Used to make the extrapolation during the interval
  // when voltage is negative
  uint32_t voltage_zero_cross_tick_count = 0;

  // Previous iteration values. Used to detect zero cross.
  fix16_t prev_voltage = 0;
  fix16_t prev_current = 0;

  uint32_t phase_counter = 0; // increment every tick
  // Holds the number of ticks per half-period (between two zero crosses)
  // Will be near 400 for 50 Hz supply voltage or near 333.3333 for 60 Hz
  // Initial value -1 prevents triac from turning on during first period
  uint32_t period_in_ticks = 0;


  bool once_zero_crossed = false;
  bool once_period_counted = false;

  // Holds number of ticks since triac is on
  uint32_t triac_on_counter = 0;

  MedianIteratorTemplate<fix16_t, 32> median_speed_filter;

  void speed_tick()
  {
    if (in_triac_on) triac_on_counter ++;
    else triac_on_counter = 0;

    // We should measure speed when triac is on and data is trustable:
    //
    // - skip couple of ticks after triac on
    // - skip everything after voltage become negative (become zero in our case)
    // - skip everything before middle of half-period to avoid measurement while
    //   negative current from previous period flows.
    if ((triac_on_counter > 3) && (voltage > 0) && (phase_counter >= period_in_ticks / 2))
    {
      fix16_t di_dt = (current - prev_current) * APP_TICK_FREQUENCY;
      fix16_t r_ekv = fix16_div(voltage, current)
        - cfg_motor_resistance
        - fix16_div(fix16_mul(cfg_motor_inductance, di_dt), current);

      fix16_t _spd_single = fix16_div(r_ekv, cfg_rekv_to_speed_factor);

      median_speed_filter.add(_spd_single);
    }

    if (zero_cross_down)
    {
      // Now we are at negative wave, update [normalized] speed
      speed = median_speed_filter.result();
      median_speed_filter.reset();
    }

  }
};


#endif
