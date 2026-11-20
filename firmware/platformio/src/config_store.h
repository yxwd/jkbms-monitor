#pragma once
// NVRAM 配置存储 —— 所有配置掉电不丢，通过 BLE 配网写入
#include <Arduino.h>
#include <Preferences.h>
#include <esp_mac.h>
#include "config.h"

struct AppConfig {
  String ssid, pass, bmsMac, deviceId, endpoint, pkey, table;
  // 涂鸦三元组与配置（PID / DeviceID / DeviceSecret 一机一密）
  String tuyaPid, tuyaDevId, tuyaSecret;
  int  tuyaRegion = TUYA_REGION_DEFAULT;
  bool tuyaEnabled = false;
  bool hasWifi() const { return ssid.length() > 0; }
  bool hasBms()  const { return bmsMac.length() > 0 && bmsMac != "00:00:00:00:00:00"; }
  bool hasTuya() const { return tuyaEnabled && tuyaPid.length() > 0 &&
                                tuyaDevId.length() > 0 && tuyaSecret.length() > 0; }
};

static Preferences _prefs;
static AppConfig gCfg;

// 芯片 MAC 后 4 位，用于广播名和默认设备 ID
static String cfgChipSuffix() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
  return String(buf);
}

static bool _isPlaceholder(const char *s) {
  return s == nullptr || strlen(s) == 0 || String(s).startsWith("YOUR_");
}

static void cfgLoad() {
  _prefs.begin("jkbms", true);
  gCfg.ssid     = _prefs.getString("ssid", _isPlaceholder(WIFI_SSID) ? "" : WIFI_SSID);
  gCfg.pass     = _prefs.getString("pass", _isPlaceholder(WIFI_PASSWORD) ? "" : WIFI_PASSWORD);
  gCfg.bmsMac   = _prefs.getString("bmsmac", _isPlaceholder(BMS_MAC) ? "" : BMS_MAC);
  gCfg.deviceId = _prefs.getString("devid", "");
  gCfg.endpoint = _prefs.getString("endpoint", CLOUD_ENDPOINT);
  gCfg.pkey     = _prefs.getString("pkey", CLOUD_PUBLISHABLE_KEY);
  gCfg.table    = _prefs.getString("table", CLOUD_TABLE);
  gCfg.tuyaPid     = _prefs.getString("typid", "");
  gCfg.tuyaDevId   = _prefs.getString("tydev", "");
  gCfg.tuyaSecret  = _prefs.getString("tysec", "");
  gCfg.tuyaRegion  = _prefs.getInt("tyregion", TUYA_REGION_DEFAULT);
  gCfg.tuyaEnabled = _prefs.getBool("tyen", TUYA_ENABLED_DEFAULT != 0);
  _prefs.end();
  if (gCfg.deviceId.length() == 0) gCfg.deviceId = "JKBMS-" + cfgChipSuffix();
}

static void cfgSave() {
  _prefs.begin("jkbms", false);
  _prefs.putString("ssid", gCfg.ssid);
  _prefs.putString("pass", gCfg.pass);
  _prefs.putString("bmsmac", gCfg.bmsMac);
  _prefs.putString("devid", gCfg.deviceId);
  _prefs.putString("endpoint", gCfg.endpoint);
  _prefs.putString("pkey", gCfg.pkey);
  _prefs.putString("table", gCfg.table);
  _prefs.putString("typid", gCfg.tuyaPid);
  _prefs.putString("tydev", gCfg.tuyaDevId);
  _prefs.putString("tysec", gCfg.tuyaSecret);
  _prefs.putInt("tyregion", gCfg.tuyaRegion);
  _prefs.putBool("tyen", gCfg.tuyaEnabled);
  _prefs.end();
}

static void cfgFactoryReset() {
  _prefs.begin("jkbms", false);
  _prefs.clear();
  _prefs.end();
}
