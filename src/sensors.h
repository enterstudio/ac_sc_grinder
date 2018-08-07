#ifndef __SENSORS__
#define __SENSORS__

#include <string.h>

#include "eeprom_float.h"
#include "config_map.h"
#include "fix16_math/fix16_math.h"
#include "app.h"

// Since we can measure only positive wave, we need record data to measure
// power. When voltage become negative, but current still flow - replay data
// from this buffer.
// Size is ~ twice more than required in worst case.
constexpr int voltage_buffer_size = APP_TICK_FREQUENCY / 50;

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

  fix16_t power = 0;
  fix16_t speed = 0;
  fix16_t voltage = 0;
  fix16_t current = 0;
  fix16_t knob = 0; // Speed knob physical value, 0..100%

  // Should be called with 40kHz frequency
  void tick()
  {
    // Do preliminary filtering of raw data + normalize result
    fetch_adc_data();

    // Poor man zero cross check (both up and down)
    if (((prev_voltage == 0) && (voltage > 0)) ||
        ((prev_voltage > 0) && (voltage == 0)))
    {
      if (once_zero_crossed) once_period_counted = true;

      once_zero_crossed = true;

      // If full half-period was counted at least once, save number of
      // ticks in half-period
      if (once_period_counted) period_in_ticks = phase_counter;

      phase_counter = 0;
    }

    // Calculate average power only if number of ticks was counted
    // in full half-period
    if (once_period_counted) power_tick();

    speed_tick();

    phase_counter++;
    prev_voltage = voltage;
  }

  // Load config from emulated EEPROM
  void configure()
  {
    cfg_power_max_inv = fix16_from_float(1.0F /
      eeprom_float_read(CFG_POWER_MAX_ADDR, CFG_POWER_MAX_DEFAULT));

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

  // Conig info
  fix16_t cfg_shunt_resistance_inv;
  fix16_t cfg_power_max_inv;
  fix16_t cfg_motor_resistance;
  fix16_t cfg_rpm_max_inv;

  // Buffer for extrapolation during the negative half-period of AC voltage
  // Record data on positive wave and replay on negative wave.
  fix16_t voltage_buffer[voltage_buffer_size];

  fix16_t p_sum = 0;

  // Holds number of ticks during the period
  // Used to calculate the average power for the period
  uint32_t power_tick_counter = 0;

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

  void power_tick()
  {
    // TODO: should detect & use phase shift

    // Positive sine wave
    if ((current > 0) && (voltage > 0))
    {
      p_sum += fix16_mul(voltage, current);
      power_tick_counter++;
    }
    // Negative sine vave => extrapolate voltage
    else if (voltage == 0)
    {
      // If this is tick when voltage crosses zero (down), save tick number
      if (prev_voltage > 0)
      {
        voltage_zero_cross_tick_count = power_tick_counter;
      }

      if (current > 0)
      {
        // Now voltage is negative, but current is still positive
        // Inductance gives power back to the supply
        // This power must be substracted from power sum
        fix16_t extrapolated_voltage = voltage_buffer[power_tick_counter - voltage_zero_cross_tick_count];

        p_sum -= fix16_mul(extrapolated_voltage, current);
        power_tick_counter++;
      }
    }

    if ((prev_current > 0) && (current == 0))
    {
      // Now we are at negative wave and shunt current ended
      // Time to calculate average power, and normalize it to [0.0..1.0]
      //
      // Normalized power = (p_sum / period_in_ticks) / cfg_power_max;
      // power > 1.0 => overload
      //
      // protect from zero div (that should never happen)
      if (period_in_ticks > 0)
      {
        power = fix16_mul(p_sum / period_in_ticks, cfg_power_max_inv);
      }
      power_tick_counter = 0;
    }

    // We should never have bufer overrun, but let's keep protection from
    // memory corruption for safety.
    if (power_tick_counter < voltage_buffer_size)
    {
      voltage_buffer[power_tick_counter] = voltage;
    }

    prev_current = current;
  }

  // Motor speed is proportional to the equivalent resistance `r_ekv`.
  // `r_ekv_sum` holds sum of calculated on each tick `r_ekv`
  // At the end of the period, arithmetic mean of `r_ekv_sum`
  // is calculated for noise reduction purpose.
  fix16_t r_ekv_sum = 0;
  // Holds number of ticks
  uint32_t speed_tick_counter = 0;


  void speed_tick()
  {
    if (voltage > 0)
    {
      if (current > 0)
      {
        // r_ekv = voltage/current - cfg_motor_resistance;
        r_ekv_sum += fix16_div(voltage, current) - cfg_motor_resistance;
        speed_tick_counter++;
      }
    }

    if ((prev_voltage > 0) && (voltage == 0))
    {
      // Now we are at negative wave, calculate [normalized] speed

      // protect from zero div (that should never happen)
      if (speed_tick_counter > 0)
      {
        fix16_t r_ekv = r_ekv_sum / speed_tick_counter;
        // TODO: hardcoded 10 => ?
        // speed = 10.0 * r_ekv / cfg_rpm_max;
        speed = fix16_mul(10 * r_ekv, cfg_rpm_max_inv);
      }
      r_ekv_sum = 0;
      speed_tick_counter = 0;
    }
  }
};


#endif
