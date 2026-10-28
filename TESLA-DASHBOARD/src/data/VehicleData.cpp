#include "data/VehicleData.h"

#include <string.h>

VehicleData g_vehicle;

void applyTesla(const TeslaPayload& p) {
  VehicleData& v = g_vehicle;
  v.speedKmh = p.speed_kmh;
  v.motorPowerKw = p.motor_power_kw;
  v.soc = p.battery_level;
  v.rangeKm = p.battery_range_km;
  v.gear = p.gear;
  memcpy(v.doors, p.doors, sizeof(v.doors));
  v.locked = p.locked != 0;
  v.charging = p.charging != 0;
  v.chargePowerKw = p.charge_power_kw;
  v.insideTempC = p.inside_temp;
  v.outsideTempC = p.outside_temp;
  memcpy(v.tpms, p.tpms, sizeof(v.tpms));

  v.valid = VF_SPEED | VF_MOTOR_POWER | VF_SOC | VF_RANGE | VF_GEAR |
            VF_DOORS | VF_LOCK | VF_CHARGING | VF_CHARGE_POWER | VF_TEMPS |
            VF_TPMS;
  v.updatedAt = millis();
}
