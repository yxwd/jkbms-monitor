#pragma once
// BLE 配网服务 —— 小程序连接后配置 WiFi / 绑定保护板 / 查看状态
// 协议见 PROTOCOL.md
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <vector>
#include "config_store.h"

#define PROV_SERVICE_UUID "0000FFF0-0000-1000-8000-00805F9B34FB"
#define PROV_WRITE_UUID   "0000FFF1-0000-1000-8000-00805F9B34FB"
#define PROV_NOTIFY_UUID  "0000FFF2-0000-1000-8000-00805F9B34FB"
// FW_VERSION 统一在 config.h 定义，OTA 与配网共用

static NimBLECharacteristic *pProvWrite = nullptr;
static NimBLECharacteristic *pProvNotify = nullptr;

// 分片重组缓冲
static String   _rxMsg;
static int      _rxNextSeq = 0;
static bool     _rxActive = false;

// 扫描请求标志（在 loop 中异步执行，避免阻塞 BLE 回调）
static volatile bool _scanRequested = false;
static volatile bool _rebootRequested = false;

// 分片发送：每帧 1 字节头 + ≤19 字节数据
static void provSend(const String &msg) {
  if (!pProvNotify) return;
  const int len = msg.length();
  const int perFrame = 19;
  int total = (len + perFrame - 1) / perFrame;
  if (total == 0) total = 1;
  if (total > 64) total = 64;  // 序号只有 6 位
  for (int i = 0; i < total; i++) {
    uint8_t hdr = 0;
    if (i == 0) hdr |= 0x80;
    if (i == total - 1) hdr |= 0x40;
    hdr |= (i & 0x3F);
    String chunk = msg.substring(i * perFrame, (i + 1) * perFrame);
    uint8_t buf[20];
    buf[0] = hdr;
    memcpy(buf + 1, chunk.c_str(), chunk.length());
    pProvNotify->notify(buf, chunk.length() + 1);
    delay(2);
  }
}

// WiFi 状态文本
static String wifiStateText() {
  if (WiFi.status() == WL_CONNECTED) return "CONNECTED";
  if (gCfg.hasWifi()) return "FAILED";
  return "IDLE";
}

// 执行扫描并在 loop 中发送结果
static void doScanAndReply() {
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(BLE_SCAN_SECONDS, false);
  NimBLEScanResults results = scan->getResults();

  String out = "BMS_LIST";
  NimBLEUUID svcUuid((uint16_t)0xFFE0);
  for (int i = 0; i < (int)results.getCount(); i++) {
    const NimBLEAdvertisedDevice *d = results.getDevice(i);
    if (!d) continue;
    bool match = false;
    if (d->haveServiceUUID()) match = d->isAdvertisingService(svcUuid);
    String name = d->haveName() ? String(d->getName().c_str()) : String("");
    if (name.indexOf("JK") >= 0 || name.indexOf("BMS") >= 0) match = true;
    if (!match) continue;
    String mac = String(d->getAddress().toString().c_str());
    out += ",";
    out += mac;
    out += "|";
    out += String(d->getRSSI());
    out += "|";
    out += name;
    if (out.length() > 7000) break;  // 防超长
  }
  provSend(out);
}

// 解析带转义的逗号分隔参数（\, 表示字面逗号）
static std::vector<String> splitArgs(const String &s) {
  std::vector<String> out;
  String cur;
  for (int i = 0; i < (int)s.length(); i++) {
    char ch = s.charAt(i);
    if (ch == '\\' && i + 1 < (int)s.length()) {
      cur += s.charAt(++i);
    } else if (ch == ',') {
      out.push_back(cur);
      cur = "";
    } else {
      cur += ch;
    }
  }
  out.push_back(cur);
  return out;
}

// 命令处理
static void handleCommand(const String &msg) {
  std::vector<String> a = splitArgs(msg);
  if (a.empty()) return;
  const String cmd = a[0];

  if (cmd == "PING") {
    provSend(String("PONG,") + FW_VERSION + "," + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("NONE")));
  } else if (cmd == "SCAN_BMS") {
    _scanRequested = true;
    // 扫描完成后在 loop 中回复 BMS_LIST
  } else if (cmd == "GET_CFG") {
    provSend(String("CFG,") + gCfg.ssid + "," + gCfg.bmsMac + "," + gCfg.deviceId + "," + wifiStateText());
  } else if (cmd == "SET_WIFI" && a.size() >= 3) {
    gCfg.ssid = a[1];
    gCfg.pass = a[2];
    cfgSave();
    provSend("OK");
    // 立即尝试连接，无需重启即可看到 IP
    if (gCfg.hasWifi()) {
      WiFi.mode(WIFI_STA);
      WiFi.begin(gCfg.ssid.c_str(), gCfg.pass.c_str());
    }
  } else if (cmd == "SET_BMS" && a.size() >= 2) {
    gCfg.bmsMac = a[1];
    gCfg.bmsMac.toUpperCase();
    cfgSave();
    provSend("OK");
  } else if (cmd == "SET_ID" && a.size() >= 2) {
    gCfg.deviceId = a[1];
    cfgSave();
    provSend("OK");
  } else if (cmd == "SET_CLOUD" && a.size() >= 4) {
    gCfg.endpoint = a[1];
    gCfg.pkey = a[2];
    gCfg.table = a[3];
    cfgSave();
    provSend("OK");
  } else if (cmd == "REBOOT") {
    provSend("OK");
    _rebootRequested = true;
  } else if (cmd == "FACTORY_RESET") {
    cfgFactoryReset();
    provSend("OK");
    _rebootRequested = true;
  } else {
    provSend("ERR:unknown command");
  }
}

class ProvWriteCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    // 每次 write 携带一个分片：[1字节头][≤19字节数据]
    std::string v = c->getValue();
    if (v.size() < 1) return;
    uint8_t hdr = (uint8_t)v[0];
    bool first = hdr & 0x80;
    bool last  = hdr & 0x40;
    int seq    = hdr & 0x3F;
    if (first) { _rxMsg = ""; _rxNextSeq = 0; _rxActive = true; }
    if (!_rxActive) return;
    if (seq != _rxNextSeq) { _rxMsg = ""; _rxActive = false; return; }  // 丢片，重置
    _rxMsg += String(v.substr(1).c_str());
    _rxNextSeq++;
    if (last) {
      handleCommand(_rxMsg);
      _rxMsg = "";
      _rxActive = false;
    }
  }
};

static void provisioningInit() {
  String advName = "JKBMS-" + cfgChipSuffix();
  NimBLEDevice::createServer();
  NimBLEService *svc = NimBLEDevice::getServer()->createService(PROV_SERVICE_UUID);
  pProvWrite = svc->createCharacteristic(PROV_WRITE_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pProvNotify = svc->createCharacteristic(PROV_NOTIFY_UUID, NIMBLE_PROPERTY::NOTIFY);
  pProvWrite->setCallbacks(new ProvWriteCb());
  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  // 用 16 位 UUID 广播：128 位 UUID 会占满 31 字节广播包，把设备名挤进 scan response，
  // 导致微信小程序拿到"未知设备"甚至空名字。16 位只占 2 字节，名字可留在广播包里。
  adv->addServiceUUID(NimBLEUUID((uint16_t)0xFFF0));
  adv->setName(advName.c_str());  // NimBLE 默认会把名字同时放进广播包和 scan response
  adv->start();
  Serial.println("Provisioning BLE adv: " + advName);
}

static void provisioningLoop() {
  // 广播保活：BLE 中央采集（连接/扫描保护板）会挤占外设配网广播，
  // 一旦广播被停掉，手机就扫不到设备。每 2 秒检查一次，掉了就重新拉起。
  static unsigned long lastAdvCheck = 0;
  if (millis() - lastAdvCheck >= 2000) {
    lastAdvCheck = millis();
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (adv && !adv->isAdvertising()) {
      adv->start();
      Serial.println("BLE advertising re-started (keepalive)");
    }
  }

  if (_scanRequested) {
    _scanRequested = false;
    doScanAndReply();
  }
  if (_rebootRequested) {
    _rebootRequested = false;
    delay(200);
    ESP.restart();
  }
}
