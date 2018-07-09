#ifndef __SPEED_CONTROLLER__
#define __SPEED_CONTROLLER__


#include "eeprom_float.h"
#include "config_map.h"
#include "utils.h"


class SpeedController
{
public:
  // Inputs
  float in_knob = 0.0;  // Knob position
  float in_speed = 0.0; // Measured speed
  float in_power = 0.0; // Measured power

  // Output power 0..100% for triac control
  float out_power = 0.0;

  // Expected be called with 100/120Hz frequency
  // More frequent calls are useless, because we can not control triac faster
  void tick()
  {
    float control_speed = calculateControlSpeed(in_knob, in_speed, lock);
    float control_power = calculateControlPower(in_power);
    out_power = minControl(control_speed, control_power);
  }

  // Load config from emulated EEPROM
  void configure()
  {
    cfg_dead_zone_width = eeprom_float_read(CFG_DEAD_ZONE_WIDTH_ADDR, CFG_DEAD_ZONE_WIDTH_DEFAULT);
    cfg_pid_p = eeprom_float_read(CFG_PID_P_ADDR, CFG_PID_P_DEFAULT);
    cfg_pid_i = eeprom_float_read(CFG_PID_I_ADDR, CFG_PID_P_DEFAULT);
    cfg_rpm_max_limit = eeprom_float_read(CFG_RPM_MAX_LIMIT_ADDR, CFG_RPM_MAX_LIMIT_DEFAULT);
    cfg_rpm_min_limit = eeprom_float_read(CFG_RPM_MIN_LIMIT_ADDR, CFG_RPM_MIN_LIMIT_DEFAULT);
    cfg_rpm_max = eeprom_float_read(CFG_RPM_MAX_ADDR, CFG_RPM_MAX_DEFAULT);
  }

private:
  // Control dead zone width near 0, when motor should not run.
  float cfg_dead_zone_width;
  // PID coefficients
  float cfg_pid_p;
  float cfg_pid_i;
  // In theory limits should be in % if max, but it's more convenient
  // for users to work with direct RPM values.
  float cfg_rpm_max_limit;
  float cfg_rpm_min_limit;
  float cfg_rpm_max;


  float PID_speed_integral = 0;
  float PID_power_integral = 0;
  bool lock = false;

  float calculateControlSpeed(float potentiometer, float speed, bool lock)
  {
    if (!lock)
    {
      if (potentiometer < cfg_dead_zone_width)
      {
        potentiometer = 0.0;
      }

      float error = potentiometer - speed;
      float proportional = cfg_pid_p*error;

      PID_speed_integral += 1.0 / cfg_pid_i * error;

      PID_speed_integral = clamp(
        PID_speed_integral,
        cfg_rpm_min_limit / cfg_rpm_max * 100.0,
        cfg_rpm_max_limit / cfg_rpm_max * 100.0
      );

      float output = proportional + PID_speed_integral;

      output = clamp(
        output,
        cfg_rpm_min_limit / cfg_rpm_max * 100.0,
        cfg_rpm_max_limit / cfg_rpm_max * 100.0
      );

      return output;
    }
    return 0.0;
  }

  float calculateControlPower(float power)
  {
    float error = 100.0 - power;
    float proportional = cfg_pid_p * error;

    PID_power_integral += 1.0 / cfg_pid_i * error;

    PID_power_integral = clamp(PID_power_integral, 0.0, 100.0);

    return clamp(proportional + PID_power_integral, 0.0, 100.0);
  }

  float minControl(float control_speed, float control_power)
  {
    if (control_speed <= control_power)
    {
      lock = false;
      return control_speed;
    }
    else
    {
      lock = true;
      return control_power;
    }
  }
};


#endif
