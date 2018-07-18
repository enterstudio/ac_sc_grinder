#ifndef __SENSORS__
#define __SENSORS__


#include "eeprom_float.h"
#include "config_map.h"

// At 40000 Hz, 1/2 of sine wave at 50Hz should take ~400 ticks to record
#define VOLTAGE_BUFFER_SIZE 800

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

  float power = 0.0;
  float speed = 0.0;
  float voltage = 0.0;
  float current = 0.0;
  float knob = 0.0; // Speed knob physical value, 0..100%

  // Should be called with 40kHz frequency
  void tick()
  {
    power_tick();
    speed_tick();
  }

  // Load config from emulated EEPROM
  void configure()
  {
    cfg_power_max = eeprom_float_read(CFG_POWER_MAX_ADDR, CFG_POWER_MAX_DEFAULT);
    cfg_motor_resistance = eeprom_float_read(CFG_MOTOR_RESISTANCE_ADDR, CFG_MOTOR_RESISTANCE_DEFAULT);
    cfg_rpm_max = eeprom_float_read(CFG_RPM_MAX_ADDR, CFG_RPM_MAX_DEFAULT);

    // config shunt resistance - in mOhm (divide by 1000)
    // shunt amplifier gain - 50
    cfg_shunt_resistance =
      eeprom_float_read(CFG_SHUNT_RESISTANCE_ADDR, CFG_SHUNT_RESISTANCE_DEFAULT)
      * 50.0
      / 1000.0;
  }

  // Store raw ADC data to normalized values
  void adc_raw_data_load(int adc_voltage, int adc_current, int adc_knob, int adc_v_refin)
  {
    // Vrefin is internal reference voltage 1.2v
    // Vref is ADC reference voltage, equal to ADC supply voltage (near 3.3v)
    // adc_vrefin = 1.2 / Vref * 4096
    float v_ref = 1.2 * 4096 / adc_v_refin;

    // 4096 - maximum value of 12-bit integer
    knob = adc_knob * (100.0 / 4096.0);

    // maximum ADC input voltage - Vref
    current = adc_current / cfg_shunt_resistance / (4096.0 / v_ref);

    // resistors in voltage divider - [ 2*150 kOhm, 1.5 kOhm ]
    // (divider ratio => 201)
    voltage = adc_voltage * v_ref / (4096.0 * 1.5 / 301.5);
  }

private:
  // Conig info
  float cfg_shunt_resistance;
  float cfg_power_max;
  float cfg_motor_resistance;
  float cfg_rpm_max;

  // Buffer for extrapolation during the negative half-period of AC voltage
  // Record data on positive wave and replay on negative wave.
  float voltage_buffer[VOLTAGE_BUFFER_SIZE];

  float p_sum = 0.0;

  // Holds number of ticks during the period
  // Used to calculate the average power for the period
  int power_tick_counter = 0;

  // Holds number of tick when voltage crosses zero
  // Used to make the extrapolation during the interval
  // when voltage is negative
  int voltage_zero_cross_tick_count = 0;

  // Previous iteration values. Used to detect zero cross.
  float prev_voltage = 0.0;
  float prev_current = 0.0;

  void power_tick()
  {
    // TODO: should detect & use phase shift

    // Positive sine wave
    if ((current > 0.0) && (voltage > 0.0))
    {
      p_sum += voltage * current;
      power_tick_counter++;
    }
    // Negative sine vave => extrapolate voltage
    else if ((current > 0.0) && (voltage == 0.0))
    {
      // If this is tick when voltage crosses zero (down), save tick number
      if (prev_voltage > 0.0)
      {
        voltage_zero_cross_tick_count = power_tick_counter;
      }

      // Now voltage is negative, but current is still positive
      // Inductance gives power back to the supply
      // This power must be substracted from power sum
      float extrapolated_voltage = voltage_buffer[power_tick_counter - voltage_zero_cross_tick_count];

      p_sum -= extrapolated_voltage * current;
      power_tick_counter++;
    }

    if ((prev_current > 0.0) && (current == 0.0))
    {
      // Now we are at negative wave and shunt current ended
      // Time to calculate average power, and convert it to % or `cfg_power_max`

      // TODO: fix logic
      power = p_sum / power_tick_counter / cfg_power_max * 100.0;
      power_tick_counter = 0;
    }

    // We should never have bufer overrun, but let's keep protection from
    // memory corruption for safety.
    if (power_tick_counter < VOLTAGE_BUFFER_SIZE)
    {
      voltage_buffer[power_tick_counter] = voltage;
    }
    prev_current = current;
  }

  // Motor speed is proportional to the equivalent resistance `r_ekv`.
  // `r_ekv_sum` holds sum of calculated on each tick `r_ekv`
  // At the end of the period, arithmetic mean of `r_ekv_sum`
  // is calculated for noise reduction purpose.
  float r_ekv_sum = 0.0;
  // Holds number of ticks
  int speed_tick_counter = 0;


  void speed_tick()
  {
    if ((current > 0.0) && (voltage > 0.0))
    {
      r_ekv_sum += voltage/current - cfg_motor_resistance;
      speed_tick_counter++;
    }

    if ((prev_voltage > 0.0) && (voltage == 0.0))
    {
      float r_ekv = r_ekv_sum / speed_tick_counter;
      // TODO: hardcoded 10 => ?
      speed = 10.0 * r_ekv / cfg_rpm_max;
      speed_tick_counter = 0.0;
    }

    prev_voltage = voltage;
  }
};


#endif
