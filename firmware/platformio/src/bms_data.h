#pragma once
#include <Arduino.h>

// 极空保护板解析后的统一数据结构（main.cpp 采集、tuya.h 上报共用）
struct BmsData {
  float totalVoltage = 0, current = 0, power = 0;
  float soc = 0, soh = 0, capRemain = 0, nomCap = 0;
  uint32_t cycles = 0, runtime = 0;
  float temps[5] = {0, 0, 0, 0, 0};
  float cells[32] = {0};
  float resist[32] = {0};
  uint8_t cellCount = 0;
  uint16_t alarms = 0;
  uint8_t balancing = 0;
  bool chargeMos = false, dischargeMos = false;
  String alarmText = "";
  String protoName = "";
  bool valid = false;
};
