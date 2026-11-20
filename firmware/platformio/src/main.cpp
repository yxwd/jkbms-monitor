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
#include "bms_data.h"
#include "provision.h"
#include "ota.h"
#include "tuya.h"

static NimBLEClient *pClient = nullptr;
static NimBLERemoteCharacteristic *pWriteChar = nullptr;
static NimBLERemoteCharacteristic *pNotifyChar = nullptr;
static bool bleReady = false;
// 采集连接健康统计（供配网 PONG/CFG 回显，验证是否反复重连）
// 非 static：provision.h 中 PONG 回显需 extern 引用
unsigned long statConnects = 0;   // 累计成功连接次数
unsigned long statDrops = 0;      // 累计被动断开次数
unsigned long statReads = 0;      // 累计读取命令次数
static unsigned long lastConnectTry = 0; // 上次发起连接的时刻（冷却用）
static unsigned long lastDropAt = 0;     // 上次断连时刻
BmsData latest;   // 非 static：tuya.h 上报模块 extern 引用
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

// JK02 帧解析（对齐 syssi/esphome-jk-bms 权威实现）
//   24S: 电芯 24 个，主字段偏移 0，电阻偏移 0
//   32S: 电芯 32 个，主字段偏移 32，电阻偏移 16，MOS 温度/告警位置特殊
// 关键：32S 的主字段偏移是 32 字节，不是 16！(之前 bug 就是只加了 16)
static void parseCellInfo(const uint8_t *d, bool is32, BmsData &out) {
  const int off = is32 ? 32 : 0;       // 主字段偏移（电压/电流/温度/SOC/容量...）
  const int roff = is32 ? 16 : 0;      // 电阻字段偏移
  out.cellCount = is32 ? 32 : 24;
  out.protoName = is32 ? "JK02_32S" : "JK02_24S";
  out.valid = true;
  // 单体电压固定在 6 起（两种协议一致）；电阻 24S@64 / 32S@80
  for (uint8_t i = 0; i < out.cellCount; i++) {
    out.cells[i] = u16(d, 6 + i * 2) * 0.001f;
    out.resist[i] = u16(d, 64 + roff + i * 2) * 0.001f;
  }
  out.totalVoltage = u32(d, 118 + off) * 0.001f;          // 24S@118 / 32S@150
  out.current = (int32_t)u32(d, 126 + off) * 0.001f;      // 24S@126 / 32S@158
  out.power = out.totalVoltage * out.current;
  out.temps[0] = (int16_t)u16(d, 130 + off) * 0.1f;       // 24S@130 / 32S@162
  out.temps[1] = (int16_t)u16(d, 132 + off) * 0.1f;       // 24S@132 / 32S@164
  if (!is32) {
    out.temps[2] = (int16_t)u16(d, 134) * 0.1f;           // 24S MOS 温度 @134
    out.alarms = (uint16_t)d[136] << 8 | d[137];          // 24S 告警 @136
  } else {
    out.temps[2] = (int16_t)u16(d, 112 + off) * 0.1f;     // 32S MOS 温度 @144
    out.alarms = (uint16_t)u32(d, 134 + off);             // 32S 告警 @166 (32位，取低16)
  }
  out.temps[3] = u16(d, 222 + off) * 0.1f;
  out.temps[4] = u16(d, 224 + off) * 0.1f;
  out.balancing = d[140 + off];                           // 24S@140 / 32S@172
  out.soc = d[141 + off];                                 // 24S@141 / 32S@173
  out.capRemain = u32(d, 142 + off) * 0.001f;             // 24S@142 / 32S@174
  out.nomCap = u32(d, 146 + off) * 0.001f;                // 24S@146 / 32S@178
  out.cycles = u32(d, 150 + off);                         // 24S@150 / 32S@182
  out.soh = d[158 + off];                                 // 24S@158 / 32S@190
  out.runtime = u32(d, 162 + off);                        // 24S@162 / 32S@194
  out.chargeMos = d[166 + off] != 0;                      // 24S@166 / 32S@198
  out.dischargeMos = d[167 + off] != 0;                   // 24S@167 / 32S@199
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

  bool is32 = false;
  if (PROTOCOL_MODE == 1) {
    is32 = false;
  } else if (PROTOCOL_MODE == 2) {
    is32 = true;
  } else {
    // 自动识别：分别按两种布局解析，取总电压合理(10~150V)的那个。
    // 32S 帧字段在 24S 位置读到的是错乱值(0 或超大)，电压不会落在合理区间。
    BmsData g24, g32;
    parseCellInfo(raw, false, g24);
    parseCellInfo(raw, true, g32);
    bool ok24 = (g24.totalVoltage > 10 && g24.totalVoltage < 150);
    bool ok32 = (g32.totalVoltage > 10 && g32.totalVoltage < 150);
    if (ok32 && !ok24) is32 = true;
    else if (ok24 && !ok32) is32 = false;
    else if (ok24 && ok32) {
      // 两者都合理时用 SOC(0~100) 与容量合理性二次判定
      bool good24 = (g24.soc <= 100 && g24.capRemain < 5000 && g24.nomCap < 5000);
      bool good32 = (g32.soc <= 100 && g32.capRemain < 5000 && g32.nomCap < 5000);
      is32 = (good32 && !good24) || (good32 && good24 && g32.cycles < 100000);
    } else {
      // 都不合理：默认按帧电芯槽位多的（32S），保证大容量板可解析
      is32 = true;
    }
  }
  parseCellInfo(raw, is32, latest);
  hasNewData = true;
  frameBuf.clear();
}

class BleClientCb : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *, int reason) override {
    if (bleReady) {           // 只统计"已建立的连接"断开，排除连接失败时的清理性断开
      statDrops++;
      lastDropAt = millis();
      Serial.printf("BMS link dropped (reason=%d, #%lu)\n", reason, statDrops);
    }
    bleReady = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    // 不删除 client：下次复用同一个句柄重连，避免反复 create/delete 触发保护板滴声
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

  lastConnectTry = millis();
  // 复用 client：只在首次创建。反复 create/delete 会让保护板每次都当作新连接"滴"一声。
  if (!pClient) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(new BleClientCb());
  }
  // 若句柄仍在连接中（如 subscribe 前状态残留），先清干净再重连
  if (pClient->isConnected()) pClient->disconnect();
  if (!pClient->connect(addr)) {
    Serial.println("BMS connect failed");
    return false;
  }
  // 请求延长 supervision timeout（4s），减少因瞬时干扰/射频争用被误断开
  pClient->updateConnParams(0x06, 0x0C, 0, 400);

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
  statConnects++;
  Serial.printf("BMS connected (#%lu, drops=%lu)\n", statConnects, statDrops);
  writeCommand(0x97);              // 设备信息
  delay(200);
  writeCommand(0x96);              // 电池信息
  statReads += 2;
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
  // 手机正在配网时暂停 WiFi 重连：WiFi 扫描会抢占 2.4G 射频，
  // 导致 BLE notify 丢包（PONG/OK 发不出去）。断开后再恢复重连。
  if (provisioningClientConnected()) return;
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
      // 断连冷却：刚掉线/刚失败后等一会儿再连，避免反复连接让保护板一直"滴"
      if (millis() - lastConnectTry >= BMS_RECONNECT_COOLDOWN_MS) {
        connectBms();
      }
      delay(200);
    } else {
      delay(200);
    }
  } else if (pClient && !pClient->isConnected()) {
    // 链路真的断了（onDisconnect 回调也会置 bleReady=false，这里是兜底双保险）
    if (!statDrops || millis() - lastDropAt > 500) { statDrops++; lastDropAt = millis(); }
    bleReady = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
  } else if (millis() - lastCmd > BLE_CMD_INTERVAL_MS) {
    // 连接保持中：只复用链路周期读取，绝不重复连接/断开
    writeCommand(0x96);
    statReads++;
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
  // 涂鸦上报（独立于 WorkBuddy 结果：WB 失败也照常尝试涂鸦）
  tuyaLoop(hasNewData);
  delay(100);
}
