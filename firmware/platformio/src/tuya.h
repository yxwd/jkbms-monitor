#pragma once
// 涂鸦智能 TuyaLink 标准 MQTT 上报（一机一密）
// 与 WorkBuddy 上报并存：两份云互不影响，任一失败不影响另一份。
//
// 协议要点（官方文档 developer.tuya.com/en/docs/iot/MQTT-protocol）:
//   Broker: m1.tuyacn.com:8883 (中国)，TLS1.2，MQTT 3.1.1，keepalive 60s
//   username = ${DeviceID}|signMethod=hmacSha256,timestamp=${10位秒},secureMode=1,accessType=1;
//   password = hex(HMAC-SHA256(key=DeviceSecret,
//                msg="deviceId=${DeviceID},timestamp=${10位秒},secureMode=1,accessType=1"))
//   上报 topic: tylink/${DeviceID}/thing/property/report
//   payload: {"msgId":"..","time":ms,"data":{"<dpCode>":{"value":x,"time":ms},...}}
//   注意 key 用 DP code（产品里定义的标识符），不是 DP id 数字。
//   必须 NTP 同步时间，否则签名时间戳不对会被拒。
//
// 依赖：PubSubClient（轻量 MQTT）+ Arduino 自带 mbedTLS(HMAC-SHA256)。

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>
#include "config.h"
#include "config_store.h"
#include "bms_data.h"
#include "mbedtls/md.h"

// main.cpp 定义，这里 extern 引用
extern BmsData latest;

// 轻量 MQTT 客户端（缓冲区 512：DP 上报 payload 足够，节省 RAM/flash）
static WiFiClientSecure _tuyaTls;
static PubSubClient _tuyaMqtt(_tuyaTls);
static unsigned long _tuyaLastTry = 0;
static unsigned long _tuyaLastReport = 0;
static bool _tuyaTimeSynced = false;

static const char* tuyaBroker() {
  switch (gCfg.tuyaRegion) {
    case 1: return "m1.tuyaus.com";       // 美西
    case 2: return "m1.tuyaeu.com";       // 中欧
    case 3: return "m1-ueaz.tuyaus.com";  // 美东
    case 4: return "m1-weaz.tuyaeu.com";  // 西欧
    case 5: return "m1.tuyain.com";       // 印度
    case 6: return "m1-sg.iotbing.com";   // 新加坡
    default: return "m1.tuyacn.com";      // 中国
  }
}

// NTP 同步（阻塞等待最多 ~3 秒），涂鸦签名必须
static bool tuyaSyncTime() {
  if (_tuyaTimeSynced && time(nullptr) > 1700000000L) return true;
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
  unsigned long t0 = millis();
  while (time(nullptr) < 1700000000L && millis() - t0 < 3000) delay(100);
  _tuyaTimeSynced = (time(nullptr) >= 1700000000L);
  Serial.printf("[TUYA] NTP %s, ts=%ld\n", _tuyaTimeSynced ? "OK" : "FAIL", (long)time(nullptr));
  return _tuyaTimeSynced;
}

// HMAC-SHA256(hex) —— mbedTLS，Arduino ESP32 自带，不额外占 flash
static String tuyaHmacHex(const String &msg, const String &key) {
  byte out[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                  (const unsigned char*)key.c_str(), key.length(),
                  (const unsigned char*)msg.c_str(), msg.length(), out);
  static const char hex[] = "0123456789abcdef";
  String s; s.reserve(64);
  for (int i = 0; i < 32; i++) { s += hex[out[i] >> 4]; s += hex[out[i] & 0xF]; }
  return s;
}

// 计算 MQTT 连接用户名/密码（每次连接重新算，因为含时间戳）
static void tuyaCredentials(String &user, String &pass) {
  long ts = (long)time(nullptr);
  user = gCfg.tuyaDevId + "|signMethod=hmacSha256,timestamp=" + String(ts) +
         ",secureMode=1,accessType=1;";
  String content = "deviceId=" + gCfg.tuyaDevId + ",timestamp=" + String(ts) +
                   ",secureMode=1,accessType=1";
  pass = tuyaHmacHex(content, gCfg.tuyaSecret);
}

// 连接涂鸦云（含 NTP、TLS、MQTT CONNECT）。不阻塞主循环太久，失败有冷却。
static bool tuyaConnect() {
  if (!gCfg.hasTuya()) return false;
  if (millis() - _tuyaLastTry < 5000) return false;   // 5 秒冷却，避免疯狂重连
  _tuyaLastTry = millis();

  if (!tuyaSyncTime()) return false;

  _tuyaTls.setInsecure();   // 涂鸦证书校验可选；ESP32 资源有限，用 TLS 加密但不校验证书链
  _tuyaMqtt.setServer(tuyaBroker(), TUYA_MQTT_PORT);
  // 关键：PubSubClient 默认缓冲仅 256 字节，装不下 11 个 DP 的 payload（约 500 字节）
  _tuyaMqtt.setBufferSize(768);
  _tuyaMqtt.setKeepAlive(TUYA_KEEPALIVE_S);
  _tuyaMqtt.setSocketTimeout(10);

  String user, pass;
  tuyaCredentials(user, pass);
  Serial.printf("[TUYA] connect %s dev=%s\n", tuyaBroker(), gCfg.tuyaDevId.c_str());
  bool ok = _tuyaMqtt.connect(gCfg.tuyaDevId.c_str(), user.c_str(), pass.c_str());
  if (ok) Serial.println("[TUYA] MQTT connected");
  else Serial.printf("[TUYA] MQTT fail state=%d\n", _tuyaMqtt.state());
  return ok;
}

// 上报一个 DP（值转成字符串），拼进 payload
static void tuyaAddDp(String &js, const char* code, const String &val, long ms, bool &first) {
  if (!first) js += ',';
  first = false;
  js += '"'; js += code; js += "\":{\"value\":";
  js += val; js += ",\"time\":"; js += String(ms); js += '}';
}
// 上报当前 BMS 读数到涂鸦（与 WorkBuddy 的 buildJson 独立）
static bool tuyaReport() {
  if (!gCfg.hasTuya()) return false;
  if (!_tuyaMqtt.connected() && !tuyaConnect()) return false;

  long ms = (long)time(nullptr) * 1000L;
  String js;
  js.reserve(420);
  js += "{\"msgId\":\"";
  js += gCfg.tuyaDevId.substring(0, 8) + String(millis());
  js += "\",\"time\":" + String(ms) + ",\"data\":{";
  bool first = true;

  // 单体极值
  float cellMax = 0, cellMin = 0;
  if (latest.cellCount > 0) {
    cellMax = cellMin = latest.cells[0];
    for (uint8_t i = 1; i < latest.cellCount; i++) {
      if (latest.cells[i] > cellMax) cellMax = latest.cells[i];
      if (latest.cells[i] > 0 && latest.cells[i] < cellMin) cellMin = latest.cells[i];
    }
  }
  tuyaAddDp(js, DP_VOLTAGE,  String(latest.totalVoltage, 2), ms, first);
  tuyaAddDp(js, DP_CURRENT,  String(latest.current, 2),      ms, first);
  tuyaAddDp(js, DP_SOC,      String((int)latest.soc),         ms, first);
  tuyaAddDp(js, DP_POWER,    String(latest.power, 1),         ms, first);
  tuyaAddDp(js, DP_CAPACITY, String(latest.capRemain, 1),     ms, first);
  tuyaAddDp(js, DP_TEMP,     String(latest.temps[0], 1),      ms, first);
  tuyaAddDp(js, DP_MOS_TEMP, String(latest.temps[2], 1),      ms, first);
  tuyaAddDp(js, DP_CELL_MAX, String(cellMax, 3),              ms, first);
  tuyaAddDp(js, DP_CELL_MIN, String(cellMin, 3),              ms, first);
  tuyaAddDp(js, DP_CYCLES,   String((unsigned long)latest.cycles), ms, first);
  tuyaAddDp(js, DP_ALARM,    String(latest.alarms != 0 ? "true" : "false"), ms, first);
  js += "}}";

  String topic = "tylink/" + gCfg.tuyaDevId + "/thing/property/report";
  bool ok = _tuyaMqtt.publish(topic.c_str(), js.c_str(), false);  // QoS 0
  Serial.printf("[TUYA] report %s (%d bytes)\n", ok ? "OK" : "FAIL", js.length());
  if (ok) _tuyaLastReport = millis();
  return ok;
}

// 主循环中调用：喂 MQTT 心跳 + 周期性上报
static void tuyaLoop(bool newData) {
  if (!gCfg.hasTuya()) return;
  if (_tuyaMqtt.connected()) {
    _tuyaMqtt.loop();   // 维持心跳、接收下行
  }
  // 有数据且距上次上报超过间隔时上报（首次立即）
  if (newData && (millis() - _tuyaLastReport >= TUYA_REPORT_INTERVAL_MS)) {
    tuyaReport();
  }
}
