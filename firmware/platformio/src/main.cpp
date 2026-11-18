/*
 * 极空 JK-B2A24S30P -> ESP32-C3 -> WorkBuddy 云服务
 * 依赖：NimBLE-Arduino (2.x)、Arduino ESP32 core 2.x/3.x
 * 配置请修改同目录 config.h
 */
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <NimBLEDevice.h>
#include <vector>
#include "config.h"
#include "config_store.h"
#include "provision.h"
#include "ota.h"

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

static NimBLEClient *pClient = nullptr;
static NimBLERemoteCharacteristic *pWriteChar = nullptr;
static NimBLERemoteCharacteristic *pNotifyChar = nullptr;
static bool bleReady = false;
static BmsData latest;
static BmsData reported;          // 上次成功上报的数据快照
static bool hasReported = false;  // 是否已成功上报过
static bool hasNewData = false;
static unsigned long lastCmd = 0, lastReport = 0, lastWifi = 0;
static std::vector<uint8_t> frameBuf;

// 与上次上报相比是否发生显著变化（决定是否需要即时上报）
static bool significantChange() {
  if (!hasReported) return true;  // 首次立即上报
  if (abs((int)latest.soc - (int)reported.soc) >= SOC_DELTA_THRESHOLD) return true;
  if (fabs(latest.totalVoltage - reported.totalVoltage) >= VOLTAGE_DELTA_THRESHOLD) return true;
  if (fabs(latest.current - reported.current) >= CURRENT_DELTA_THRESHOLD) return true;
  if (fabs(latest.temps[0] - reported.temps[0]) >= TEMP_DELTA_THRESHOLD) return true;
  if (latest.alarms != reported.alarms) return true;
  if (latest.chargeMos != reported.chargeMos || latest.dischargeMos != reported.dischargeMos) return true;
  return false;
}

static uint16_t u16(const uint8_t *d, int i) { return (uint16_t)d[i + 1] << 8 | d[i]; }
static uint32_t u32(const uint8_t *d, int i) {
  return (uint32_t)u16(d, i + 2) << 16 | u16(d, i);
}
static uint8_t crc8(const uint8_t *d, int len) {
  uint8_t c = 0;
  for (int i = 0; i < len; i++) c += d[i];
  return c;
}

static String alarmToString(uint16_t m) {
  static const char *names[16] = {
    "充电过温", "充电低温", "协处理器通信错误", "单体欠压",
    "整包欠压", "放电过流", "放电短路", "放电过温",
    "线阻异常", "MOS过温", "节数与设置不符", "电流传感器异常",
    "单体过压", "整包过压", "充电过流", "充电短路"
  };
  String s = "";
  for (int i = 0; i < 16; i++) {
    if (m & (1u << i)) {
      if (s.length()) s += "|";
      s += names[i];
    }
  }
  return s.length() ? s : "正常";
}

// offset = 0 -> JK02_24S；offset = 16（字段整体 +32 字节）-> JK02_32S
static void parseCellInfo(const uint8_t *d, int offset, BmsData &out) {
  out.cellCount = (offset == 0) ? 24 : 32;
  out.protoName = (offset == 0) ? "JK02_24S" : "JK02_32S";
  out.valid = true;
  for (uint8_t i = 0; i < out.cellCount; i++) {
    out.cells[i] = u16(d, 6 + i * 2) * 0.001f;
    out.resist[i] = u16(d, 64 + offset + i * 2) * 0.001f;
  }
  out.totalVoltage = u32(d, 118 + offset) * 0.001f;
  out.current = (int32_t)u32(d, 126 + offset) * 0.001f;
  out.power = out.totalVoltage * out.current;
  out.temps[0] = (int16_t)u16(d, 130 + offset) * 0.1f;
  out.temps[1] = (int16_t)u16(d, 132 + offset) * 0.1f;
  if (offset == 0) {
    out.temps[2] = (int16_t)u16(d, 134) * 0.1f;                    // MOS 温度
    out.alarms = (uint16_t)d[136] << 8 | d[137];
  } else {
    out.temps[2] = (int16_t)u16(d, 112 + offset) * 0.1f;           // 32S 的 MOS 温度位置不同
    out.alarms = (uint16_t)d[134 + offset] << 8 | d[135 + offset];
  }
  out.temps[3] = u16(d, 222 + offset) * 0.1f;
  out.temps[4] = u16(d, 224 + offset) * 0.1f;
  out.balancing = d[140 + offset];
  out.soc = d[141 + offset];
  out.capRemain = u32(d, 142 + offset) * 0.001f;
  out.nomCap = u32(d, 146 + offset) * 0.001f;
  out.cycles = u32(d, 150 + offset);
  out.soh = d[158 + offset];
  out.runtime = u32(d, 162 + offset);
  out.chargeMos = d[166 + offset] != 0;
  out.dischargeMos = d[167 + offset] != 0;
  out.alarmText = alarmToString(out.alarms);
}

static void onNotifyData(uint8_t *data, size_t len) {
  if (len >= 4 && data[0] == 0x55 && data[1] == 0xAA &&
      data[2] == 0xEB && data[3] == 0x90) {
    frameBuf.clear();
  }
  for (size_t i = 0; i < len; i++) {
    frameBuf.push_back(data[i]);
    if (frameBuf.size() > 320) { frameBuf.clear(); break; }
  }
  if (frameBuf.size() < 300) return;

  const uint8_t *raw = frameBuf.data();
  if (crc8(raw, 299) != raw[299] || raw[4] != 0x02) {
    frameBuf.clear();
    return;
  }

  BmsData guess;
  int offset = 0;
  if (PROTOCOL_MODE == 1) {
    offset = 0;
  } else if (PROTOCOL_MODE == 2) {
    offset = 16;
  } else {
    parseCellInfo(raw, 0, guess);
    uint8_t n = 0;
    for (int i = 0; i < 24; i++) if (guess.cells[i] > 0) n++;
    if (!(guess.totalVoltage > 10 && guess.totalVoltage < 150 && n >= 8)) offset = 16;
  }
  parseCellInfo(raw, offset, latest);
  hasNewData = true;
  frameBuf.clear();
}

class BleClientCb : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *, int) override {
    bleReady = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
  }
};

// NimBLE-Arduino 2.x 用 subscribe() 传入 notify_callback，不再用 setCallbacks 类

static void writeCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t f[20] = {0};
  f[0] = 0xAA; f[1] = 0x55; f[2] = 0x90; f[3] = 0xEB;
  f[4] = cmd; f[5] = 0;
  f[19] = crc8(f, 19);
  pWriteChar->writeValue(f, 20, false);
}

static bool connectBms() {
  NimBLEAddress addr;
  if (gCfg.hasBms()) {
    // 已绑定：精确连接指定 MAC，绝不扫描选最强，杜绝误连相邻保护板
    addr = NimBLEAddress(std::string(gCfg.bmsMac.c_str()), BLE_ADDR_PUBLIC);
  } else {
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    scan->start(BLE_SCAN_SECONDS, false);
    NimBLEScanResults results = scan->getResults();
    int bestRssi = -127;
    bool found = false;
    NimBLEUUID svcUuid((uint16_t)0xFFE0);
    for (int i = 0; i < (int)results.getCount(); i++) {
      const NimBLEAdvertisedDevice *adv = results.getDevice(i);
      if (!adv) continue;
      bool match = false;
      if (adv->haveServiceUUID()) match = adv->isAdvertisingService(svcUuid);
      String name = adv->haveName() ? String(adv->getName().c_str()) : String("");
      if (name.indexOf("JK") >= 0 || name.indexOf("BMS") >= 0) match = true;
      if (match && adv->getRSSI() > bestRssi) {
        bestRssi = adv->getRSSI();
        addr = adv->getAddress();
        found = true;
      }
    }
    if (!found) return false;
  }

  pClient = NimBLEDevice::createClient();
  pClient->setClientCallbacks(new BleClientCb());
  if (!pClient->connect(addr)) return false;
  NimBLERemoteService *svc = pClient->getService(NimBLEUUID((uint16_t)0xFFE0));
  if (!svc) { pClient->disconnect(); return false; }

  NimBLEUUID charUuid((uint16_t)0xFFE1);
  const std::vector<NimBLERemoteCharacteristic *> &chars = svc->getCharacteristics(true);
  for (auto c : chars) {
    if (!c->getUUID().equals(charUuid)) continue;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }
  if (!pWriteChar) pWriteChar = pNotifyChar;
  if (!pNotifyChar || !pWriteChar) { pClient->disconnect(); return false; }

  pNotifyChar->subscribe(true, [](NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
    onNotifyData(data, len);
  }, false);
  bleReady = true;
  writeCommand(0x97);              // 设备信息
  delay(200);
  writeCommand(0x96);              // 电池信息
  lastCmd = millis();
  return true;
}

static String buildJson() {
  String js = "{";
  js += "\"device_id\":\"" + gCfg.deviceId + "\",";
  js += "\"fw_version\":\"" + String(FW_VERSION) + "\",";
  js += "\"protocol\":\"" + (latest.protoName.length() ? latest.protoName : String("auto")) + "\",";
  js += "\"total_voltage\":" + String(latest.totalVoltage, 3) + ",";
  js += "\"current\":" + String(latest.current, 3) + ",";
  js += "\"power\":" + String(latest.power, 2) + ",";
  js += "\"soc\":" + String((int)latest.soc) + ",";
  js += "\"soh\":" + String((int)latest.soh) + ",";
  js += "\"capacity_remain\":" + String(latest.capRemain, 3) + ",";
  js += "\"nominal_capacity\":" + String(latest.nomCap, 3) + ",";
  js += "\"cycles\":" + String(latest.cycles) + ",";
  js += "\"runtime_s\":" + String(latest.runtime) + ",";
  js += "\"temp1\":" + String(latest.temps[0], 1) + ",";
  js += "\"temp2\":" + String(latest.temps[1], 1) + ",";
  js += "\"mos_temp\":" + String(latest.temps[2], 1) + ",";
  js += "\"temp3\":" + String(latest.temps[3], 1) + ",";
  js += "\"temp4\":" + String(latest.temps[4], 1) + ",";
  js += "\"cells\":[";
  for (int i = 0; i < latest.cellCount; i++) {
    if (i) js += ",";
    js += String(latest.cells[i], 3);
  }
  js += "],\"resistances\":[";
  for (int i = 0; i < latest.cellCount; i++) {
    if (i) js += ",";
    js += String(latest.resist[i], 3);
  }
  js += "],\"alarms\":" + String(latest.alarms) + ",";
  js += "\"alarm_text\":\"" + latest.alarmText + "\",";
  js += "\"balancing\":" + String((int)latest.balancing) + ",";
  js += "\"charge_mos\":" + String(latest.chargeMos ? "true" : "false") + ",";
  js += "\"discharge_mos\":" + String(latest.dischargeMos ? "true" : "false");
  js += "}";
  return js;
}

static bool upload() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = gCfg.endpoint + "/.cloud/database/rest/" + gCfg.table;
  http.begin(client, url);
  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-wb-webapp-access-key", gCfg.pkey);
  http.addHeader("Prefer", "return=minimal");
  int code = http.POST(buildJson());
  http.end();
  return code >= 200 && code < 300;
}

static void ensureWifi() {
  if (!gCfg.hasWifi()) return;             // 未配置，等配网
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifi < 10000) return; // 10 秒重试一次
  lastWifi = millis();
  WiFi.reconnect();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // 从 NVRAM 加载配置
  cfgLoad();
  otaInit();   // 确认上次 OTA 有效，取消 bootloader 回滚标记
  Serial.println("Config loaded: WiFi=" + (gCfg.hasWifi() ? gCfg.ssid : "NONE") +
                 " BMS=" + (gCfg.hasBms() ? gCfg.bmsMac : "NONE") +
                 " ID=" + gCfg.deviceId);

  // 【关键修复】BLE 配网广播必须在 WiFi 之前启动。
  // 旧版先阻塞等 WiFi（最多 15 秒）再初始化 BLE，导致"保存 WiFi 重启后
  // 立刻扫码"落在空窗期扫不到设备。现在设备一上电就广播，立即可配网。
  NimBLEDevice::init("JKBMS-ESP32C3");
  NimBLEDevice::setPower(ESP_PWR_LVL_P6);
  provisioningInit();
  Serial.println("BLE init done, provisioning advertising started");

  // WiFi 后台非阻塞连接：begin() 立即返回，连不连得上由 loop 里的 ensureWifi() 负责重试，
  // 绝不在这里阻塞等待，避免拖慢 BLE 就绪。
  WiFi.mode(WIFI_STA);
  if (gCfg.hasWifi()) {
    WiFi.begin(gCfg.ssid.c_str(), gCfg.pass.c_str());
    Serial.println("WiFi connecting in background: " + gCfg.ssid);
  } else {
    Serial.println("WiFi not configured -> wait for BLE provisioning");
  }
}

void loop() {
  provisioningLoop();   // 处理配网扫描/重启请求
  ensureWifi();
  otaCheck();           // 周期性检查固件新版本并自动升级

  if (!bleReady) {
    // 未绑定保护板时不主动扫描，避免与配网争用 BLE；等待小程序绑定 MAC
    if (gCfg.hasBms()) {
      if (connectBms()) Serial.println("BMS connected");
      else delay(1000);
    } else {
      delay(200);
    }
  } else if (pClient && !pClient->isConnected()) {
    bleReady = false;
  } else if (millis() - lastCmd > BLE_CMD_INTERVAL_MS) {
    writeCommand(0x96);
    lastCmd = millis();
  }

  if (latest.valid) {
    unsigned long sinceReport = millis() - lastReport;
    bool changed = hasNewData && significantChange();
    // 显著变化：首次立即报，之后最快 CHANGE_REPORT_MIN_MS 一次
    bool changeDue = changed && (!hasReported || sinceReport >= CHANGE_REPORT_MIN_MS);
    // 心跳：数据长期稳定也定期上报一次，证明设备在线
    bool heartbeatDue = sinceReport >= REPORT_INTERVAL_MS;

    if (changeDue || heartbeatDue) {
      bool ok = upload();
      Serial.printf("Upload %s (%s): V=%.2f I=%.2f SOC=%.0f cells=%d\n",
                    ok ? "OK" : "FAIL", changeDue ? "change" : "heartbeat",
                    latest.totalVoltage, latest.current, latest.soc, latest.cellCount);
      if (ok) {
        reported = latest;   // 保存快照作为下次对比基准
        hasReported = true;
        hasNewData = false;
        lastReport = millis();
      }
    }
  }
  delay(100);
}
