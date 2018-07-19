#ifndef __SPEED_CONTROLLER__
#define __SPEED_CONTROLLER__


#include "eeprom_float.h"
#include "config_map.h"
#include "utils.h"
#include "fix16_math/fix16_math.h"


class SpeedController
{
public:
  // Inputs
  fix16_t in_knob = 0;  // Knob position
  fix16_t in_speed = 0; // Measured speed
  fix16_t in_power = 0; // Measured power

  // Output power 0..100% for triac control
  fix16_t out_power = 0;

  // Expected be called with 100/120Hz frequency
  // More frequent calls are useless, because we can not control triac faster

  // Contains two PIDs. pid_speed used in normal mode, pid_power - in power limit mode.
  // When motor power exceeds the limit, the pid_power output drops below
  // pid_speed output.
  void tick()
  {
    if (in_knob < fix16_cfg_dead_zone_width) fix16_knob_normalized = 0;
    else
    {
      // / (100.0 - cfg_dead_zone_width)
      // * (out_max_clamp - out_min_clamp)
      fix16_knob_normalized = fix16_mul((in_knob - fix16_cfg_dead_zone_width),
       fix16_knob_norm_coeff) + fix16_out_min_clamp;
    }

    if (!power_limit)
    {
      fix16_pid_speed_out = speed_pid_tick();
    }

    fix16_t fix16_pid_power_out = power_pid_tick();

    if (fix16_pid_speed_out <= fix16_pid_power_out)
    {
      if (power_limit)
      {
        // Recalculate PID_speed_integral to ensure smooth switch to normal mode
        fix16_pid_speed_integral = fix16_pid_speed_out -
         fix16_mul((fix16_knob_normalized - in_speed), fix16_cfg_pid_p);
        power_limit = false;
      }
      out_power = fix16_pid_speed_out;
    }
    else
    {
      power_limit = true;
      out_power = fix16_pid_power_out;
    }
  }

  // Load config from emulated EEPROM
  void configure()
  {
    fix16_cfg_dead_zone_width = fix16_from_float(eeprom_float_read(CFG_DEAD_ZONE_WIDTH_ADDR,
       CFG_DEAD_ZONE_WIDTH_DEFAULT));
    fix16_cfg_pid_p = fix16_from_float(eeprom_float_read(CFG_PID_P_ADDR,
       CFG_PID_P_DEFAULT));
    fix16_cfg_pid_i_inv = fix16_from_float(1.0 / eeprom_float_read(CFG_PID_I_ADDR,
       CFG_PID_P_DEFAULT));
    fix16_cfg_rpm_max_limit = fix16_from_float(eeprom_float_read(CFG_RPM_MAX_LIMIT_ADDR,
       CFG_RPM_MAX_LIMIT_DEFAULT));
    fix16_cfg_rpm_min_limit = fix16_from_float(eeprom_float_read(CFG_RPM_MIN_LIMIT_ADDR,
       CFG_RPM_MIN_LIMIT_DEFAULT));
    fix16_cfg_rpm_max = fix16_from_float(eeprom_float_read(CFG_RPM_MAX_ADDR,
       CFG_RPM_MAX_DEFAULT));

    fix16_out_min_clamp = fix16_div(fix16_cfg_rpm_min_limit, fix16_cfg_rpm_max);
    fix16_out_max_clamp = fix16_div(fix16_cfg_rpm_max_limit, fix16_cfg_rpm_max);
    fix16_knob_norm_coeff =  fix16_div((fix16_out_max_clamp - fix16_out_min_clamp),
     (fix16_one - fix16_cfg_dead_zone_width));
  }

private:
  // Control dead zone width near 0, when motor should not run.
  fix16_t fix16_cfg_dead_zone_width;
  // PID coefficients
  fix16_t fix16_cfg_pid_p;
  fix16_t fix16_cfg_pid_i_inv;
  // In theory limits should be in % if max, but it's more convenient
  // for users to work with direct RPM values.
  fix16_t fix16_cfg_rpm_max_limit;
  fix16_t fix16_cfg_rpm_min_limit;
  fix16_t fix16_cfg_rpm_max;

  // Cache for clamping limits & normalization, calculated on config load
  fix16_t fix16_out_min_clamp = 0;
  fix16_t fix16_out_max_clamp = 1;
  fix16_t fix16_knob_norm_coeff = 1;


  fix16_t fix16_pid_speed_integral = 0;
  fix16_t fix16_pid_power_integral = 0;
  fix16_t fix16_pid_speed_out = 0;
  bool power_limit = false;

  // knob value normalized to range (cfg_rpm_min_limit..cfg_rpm_max_limit)
  fix16_t fix16_knob_normalized;

  float speed_pid_tick()
  {
    // float divergence = knob_normalized - in_speed;
    fix16_t fix16_divergence = fix16_knob_normalized - in_speed;

    // TODO: ???? cfg_pid_i = 0 => result = infinity
    // TODO: cache division
    // pid_speed_integral += 1.0 / cfg_pid_i * divergence;
    fix16_pid_speed_integral += fix16_mul(fix16_cfg_pid_i_inv, fix16_divergence);
    // pid_speed_integral = clamp(pid_speed_integral, out_min_clamp, out_max_clamp);
    fix16_pid_speed_integral = fix16_clamp(fix16_pid_speed_integral,
       fix16_out_min_clamp, fix16_out_max_clamp);

    fix16_t fix16_proportional = fix16_mul(fix16_cfg_pid_p, fix16_divergence);

    return fix16_clamp(fix16_proportional + fix16_pid_speed_integral,
       fix16_out_min_clamp, fix16_out_max_clamp);
  }

  float power_pid_tick()
  {
    // float divergence = 100.0 - in_power;
    fix16_t fix16_divergence = fix16_one - in_power;

    // TODO: ???? cfg_pid_i = 0 => result = infinity
    // TODO: cache division
    // pid_power_integral += 1.0 / cfg_pid_i * divergence;
    fix16_pid_power_integral += fix16_mul(fix16_cfg_pid_i_inv, fix16_divergence);
    fix16_pid_power_integral = fix16_clamp_zero_one(fix16_pid_power_integral);

    fix16_t fix16_proportional = fix16_mul(fix16_cfg_pid_p, fix16_divergence);

    return fix16_clamp_zero_one(fix16_proportional + fix16_pid_power_integral);
  }
};


#endif
