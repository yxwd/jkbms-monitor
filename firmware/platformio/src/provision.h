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

// 采集链路统计（main.cpp 定义），PONG/CFG 回显供小程序验证连接复用
extern unsigned long statConnects, statDrops, statReads;

static NimBLECharacteristic *pProvWrite = nullptr;
static NimBLECharacteristic *pProvNotify = nullptr;

// 手机是否正在配网连接。连接期间暂停 WiFi 重连，避免 WiFi 扫描抢占射频导致 BLE 丢包。
static volatile bool _provClientConnected = false;
inline bool provisioningClientConnected() { return _provClientConnected; }

// 当前配网连接的句柄。notify 时精确发给这一个客户端，避免广播式 notify 在多连接时发错/丢失。
static volatile uint16_t _provConnHandle = BLE_HS_CONN_HANDLE_NONE;

// 最新一条回复的完整文本。FFF2 同时提供 READ 属性，小程序在 notify 收不到时可主动读取兜底。
static String _lastReply = "NONE";

class ProvServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &info) override {
    _provClientConnected = true;
    _provConnHandle = info.getConnHandle();
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
    _provClientConnected = false;
    _provConnHandle = BLE_HS_CONN_HANDLE_NONE;
  }
};

// FFF2 读回调：返回最新回复文本，作为 notify 丢失时的兜底通道
class ProvReadCb : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    c->setValue(_lastReply.c_str());
  }
};

// 分片重组缓冲
static String   _rxMsg;
static int      _rxNextSeq = 0;
static bool     _rxActive = false;

// 扫描/重启请求标志（在 loop 中异步执行，避免阻塞 BLE 回调）
static volatile bool _scanRequested = false;
static volatile bool _rebootRequested = false;

// 待发送回复队列：GATT 回调(onWrite)上下文中直接 notify 极易丢包
// （尤其 WiFi 正在扫描/重连抢占射频时），统一入队后由 provisioningLoop 在主循环发送。
static String _replyQueue[8];
static int    _replyHead = 0;
static int    _replyTail = 0;
static void queueReply(const String &msg) {
  int next = (_replyTail + 1) % 8;
  if (next == _replyHead) return;   // 队列满，丢弃最旧策略下直接放弃
  _replyQueue[_replyTail] = msg;
  _replyTail = next;
}

// 分片发送：每帧 1 字节头 + ≤19 字节数据
static void provSend(const String &msg) {
  if (!pProvNotify) return;
  // 保存完整回复，供小程序 READ 兜底（notify 万一丢失也能读到）
  _lastReply = msg;
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
    // 精确发给当前配网客户端；无有效句柄时退回广播式 notify
    if (_provConnHandle != BLE_HS_CONN_HANDLE_NONE) {
      pProvNotify->notify(buf, chunk.length() + 1, _provConnHandle);
    } else {
      pProvNotify->notify(buf, chunk.length() + 1);
    }
    delay(3);
  }
}

// WiFi 状态文本
static String wifiStateText() {
  if (WiFi.status() == WL_CONNECTED) return "CONNECTED";
  if (gCfg.hasWifi()) return "FAILED";
  return "IDLE";
}

// 执行扫描并在 loop 中发送结果（带逐台诊断日志，方便调试为什么没匹配到保护板）
static void doScanAndReply() {
  // 起始提示：让用户/小程序知道 ESP32 确实开始扫描了（扫描期间主循环阻塞 10 秒无其他回显）
  provSend("SCANLOG|start");
  Serial.println("[SCAN] start, duration=" + String(BLE_SCAN_SECONDS) + "s");

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(BLE_SCAN_SECONDS, false);
  NimBLEScanResults results = scan->getResults();

  String out = "BMS_LIST";           // 原有结果消息，保持协议不变
  String rows = "";                  // 诊断明细行（纯 ASCII，避免中文跨帧解码问题）
  NimBLEUUID svcUuid((uint16_t)0xFFE0);
  const int total = results.getCount();
  int hit = 0, shown = 0;
  for (int i = 0; i < total; i++) {
    const NimBLEAdvertisedDevice *d = results.getDevice(i);
    if (!d) continue;
    bool svcHit = false;
    if (d->haveServiceUUID()) svcHit = d->isAdvertisingService(svcUuid);
    String nameFull = d->haveName() ? String(d->getName().c_str()) : String("");
    bool nameHit = (nameFull.indexOf("JK") >= 0 || nameFull.indexOf("BMS") >= 0);
    bool match = svcHit || nameHit;
    int rssi = d->getRSSI();
    String mac = String(d->getAddress().toString().c_str());
    String nameShort = nameFull.length() ? nameFull : String("(noname)");
    if (nameShort.length() > 16) nameShort = nameShort.substring(0, 16);
    String mac6 = mac.length() >= 8 ? mac.substring(mac.length() - 8) : mac;

    // 串口详细日志
    Serial.println("[SCAN] #" + String(i + 1) + " " + nameFull + " " + mac +
                   " rssi=" + String(rssi) + " svc=" + (svcHit ? "Y" : "N") +
                   " name=" + (nameHit ? "Y" : "N") + " -> " + (match ? "HIT" : "skip"));

    if (match) {
      hit++;
      out += ",";
      out += mac;
      out += "|";
      out += String(rssi);
      out += "|";
      out += nameFull;
      if (out.length() > 7000) break;  // 防超长
    }
    // 诊断行：序号|名字|MAC后8位|RSSI|svc命中|name命中|最终判定
    // 限制 24 行，防止分片数超过协议上限（6位序号=64帧）
    if (shown < 24) {
      rows += String(i + 1) + "|" + nameShort + "|" + mac6 + "|" + String(rssi) +
              "|" + (svcHit ? "Y" : "N") + "|" + (nameHit ? "Y" : "N") +
              "|" + (match ? "HIT" : "skip") + "\n";
      shown++;
    }
  }

  // 诊断汇总：首行 total|hit，其后每行一台设备
  String diag = "SCANLOG|" + String(total) + "|" + String(hit) + "\n" + rows;
  provSend(diag);       // 逐台诊断发给小程序日志区
  provSend(out);        // 原有 BMS_LIST，保持兼容
  Serial.println("[SCAN] done: total=" + String(total) + " hit=" + String(hit));
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
    // 附带采集链路统计：C=成功连接次数 D=断连次数 R=读取次数，便于验证"连接复用、不反复重连"
    queueReply(String("PONG,") + FW_VERSION + "," + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("NONE")) +
               ",C" + String(statConnects) + "/D" + String(statDrops) + "/R" + String(statReads));
  } else if (cmd == "SCAN_BMS") {
    _scanRequested = true;
    // 扫描完成后在 loop 中回复 BMS_LIST
  } else if (cmd == "GET_CFG") {
    queueReply(String("CFG,") + gCfg.ssid + "," + gCfg.bmsMac + "," + gCfg.deviceId + "," + wifiStateText() +
               ",C" + String(statConnects) + "/D" + String(statDrops) + "/R" + String(statReads));
  } else if (cmd == "SET_WIFI" && a.size() >= 3) {
    gCfg.ssid = a[1];
    gCfg.pass = a[2];
    cfgSave();
    queueReply("OK");
    // 立即尝试连接，无需重启即可看到 IP
    if (gCfg.hasWifi()) {
      WiFi.mode(WIFI_STA);
      WiFi.begin(gCfg.ssid.c_str(), gCfg.pass.c_str());
    }
  } else if (cmd == "SET_BMS" && a.size() >= 2) {
    gCfg.bmsMac = a[1];
    gCfg.bmsMac.toUpperCase();
    cfgSave();
    queueReply("OK");
  } else if (cmd == "SET_ID" && a.size() >= 2) {
    gCfg.deviceId = a[1];
    cfgSave();
    queueReply("OK");
  } else if (cmd == "SET_CLOUD" && a.size() >= 4) {
    gCfg.endpoint = a[1];
    gCfg.pkey = a[2];
    gCfg.table = a[3];
    cfgSave();
    queueReply("OK");
  } else if (cmd == "REBOOT") {
    queueReply("OK");
    _rebootRequested = true;
  } else if (cmd == "FACTORY_RESET") {
    cfgFactoryReset();
    queueReply("OK");
    _rebootRequested = true;
  } else {
    queueReply("ERR:unknown command");
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
  NimBLEDevice::getServer()->setCallbacks(new ProvServerCb());
  NimBLEService *svc = NimBLEDevice::getServer()->createService(PROV_SERVICE_UUID);
  pProvWrite = svc->createCharacteristic(PROV_WRITE_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  // FFF2 同时支持 NOTIFY + READ：notify 丢失时小程序可主动 read 读取 _lastReply 兜底
  pProvNotify = svc->createCharacteristic(PROV_NOTIFY_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
  pProvWrite->setCallbacks(new ProvWriteCb());
  pProvNotify->setCallbacks(new ProvReadCb());
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

  // 发送队列中的回复。必须在主循环里 notify：在 GATT onWrite 回调上下文中
  // 直接 notify 会丢包（WiFi 扫描/重连抢占射频时尤其严重），导致小程序收不到 PONG/OK。
  if (_replyHead != _replyTail) {
    String reply = _replyQueue[_replyHead];
    _replyHead = (_replyHead + 1) % 8;
    provSend(reply);
  }

  if (_scanRequested) {
    _scanRequested = false;
    doScanAndReply();
  }
  if (_rebootRequested) {
    _rebootRequested = false;
    delay(300);  // 给队列里的 OK 留出发送时间
    ESP.restart();
  }
}
