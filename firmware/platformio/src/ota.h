#pragma once
// OTA 远程升级：心跳间隙轮询云端版本表，HTTPS 流式下载 + SHA256 校验后写入备用分区
// 固件包必须放在稳定的公开 HTTPS 地址；云存储签名链接会过期，不能用于 OTA
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <ctype.h>
#include <vector>
#include "config_store.h"

#ifndef FW_VERSION
#define FW_VERSION "1.2.0"
#endif
#ifndef FW_VERSION_CODE
#define FW_VERSION_CODE 10200
#endif
#ifndef OTA_CHECK_INTERVAL_MS
#define OTA_CHECK_INTERVAL_MS 300000
#endif

static unsigned long _lastOtaCheck = 0;
static bool _otaBusy = false;

static bool _shaStart(mbedtls_sha256_context *ctx) {
  mbedtls_sha256_init(ctx);
#if defined(MBEDTLS_VERSION_NUMBER) && MBEDTLS_VERSION_NUMBER >= 0x03000000
  return mbedtls_sha256_starts_ret(ctx, 0) == 0;
#else
  mbedtls_sha256_starts(ctx, 0);
  return true;
#endif
}

static bool _shaUpdate(mbedtls_sha256_context *ctx, const uint8_t *data, size_t len) {
#if defined(MBEDTLS_VERSION_NUMBER) && MBEDTLS_VERSION_NUMBER >= 0x03000000
  return mbedtls_sha256_update_ret(ctx, data, len) == 0;
#else
  mbedtls_sha256_update(ctx, data, len);
  return true;
#endif
}

static bool _shaFinish(mbedtls_sha256_context *ctx, uint8_t out[32]) {
#if defined(MBEDTLS_VERSION_NUMBER) && MBEDTLS_VERSION_NUMBER >= 0x03000000
  return mbedtls_sha256_finish_ret(ctx, out) == 0;
#else
  mbedtls_sha256_finish(ctx, out);
  return true;
#endif
}

static String _hex32(const uint8_t *d) {
  static const char *h = "0123456789abcdef";
  String s;
  s.reserve(64);
  for (int i = 0; i < 32; i++) {
    s += h[(d[i] >> 4) & 0x0F];
    s += h[d[i] & 0x0F];
  }
  return s;
}

static String _jsonStr(const String &obj, const char *key) {
  String k = String("\"") + key + "\":";
  int p = obj.indexOf(k);
  if (p < 0) return String("");
  p += k.length();
  while (p < obj.length() && isspace((unsigned char)obj.charAt(p))) p++;
  if (p >= obj.length() || obj.charAt(p) != '"') return String("");
  p++;
  int e = obj.indexOf('"', p);
  if (e < 0) return String("");
  return obj.substring(p, e);
}

static int _jsonInt(const String &obj, const char *key) {
  String k = String("\"") + key + "\":";
  int p = obj.indexOf(k);
  if (p < 0) return 0;
  p += k.length();
  while (p < obj.length() && isspace((unsigned char)obj.charAt(p))) p++;
  int e = p;
  while (e < obj.length() &&
         (isdigit((unsigned char)obj.charAt(e)) || obj.charAt(e) == '-')) e++;
  return obj.substring(p, e).toInt();
}

static std::vector<String> _jsonObjects(const String &json) {
  std::vector<String> out;
  int depth = 0, start = -1;
  bool inStr = false;
  for (int i = 0; i < json.length(); i++) {
    char c = json.charAt(i);
    if (c == '"') {
      if (i == 0 || json.charAt(i - 1) != '\\') inStr = !inStr;
      continue;
    }
    if (inStr) continue;
    if (c == '{') {
      if (depth == 0) start = i;
      depth++;
    } else if (c == '}') {
      depth--;
      if (depth == 0 && start >= 0) {
        out.push_back(json.substring(start, i + 1));
        start = -1;
      }
    }
  }
  return out;
}

// target_devices: "*" 表示全部；否则是逗号分隔的 deviceId 列表
static bool _targetMatch(const String &target, const String &devId) {
  String t = target;
  t.trim();
  if (t == "*") return true;
  if (t.length() == 0 || devId.length() == 0) return false;
  int start = 0;
  while (start <= t.length()) {
    int comma = t.indexOf(',', start);
    String part = (comma < 0) ? t.substring(start) : t.substring(start, comma);
    part.trim();
    if (part.equalsIgnoreCase(devId)) return true;
    if (comma < 0) break;
    start = comma + 1;
  }
  return false;
}

static bool _downloadAndFlash(const String &url, const String &shaHex, int sizeBytes) {
  if (sizeBytes <= 0 || sizeBytes > (int)ESP.getFreeSketchSpace()) {
    Serial.printf("OTA: bad size %d (free %u)\n", sizeBytes, ESP.getFreeSketchSpace());
    return false;
  }
  if (shaHex.length() != 64) {
    Serial.println("OTA: bad sha256 length");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();          // 证书不校验，靠 SHA256 保证固件完整
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(30000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("OTA: HTTP %d\n", code);
    http.end();
    return false;
  }
  if (!Update.begin(sizeBytes, U_FLASH)) {
    Serial.println("OTA: Update.begin failed");
    http.end();
    return false;
  }

  mbedtls_sha256_context ctx;
  if (!_shaStart(&ctx)) {
    Update.abort();
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  static uint8_t buf[4096];
  int total = 0;
  unsigned long t0 = millis();
  bool ok = true;

  while (total < sizeBytes && millis() - t0 < 180000) {
    int avail = stream->available();
    if (avail <= 0) { delay(10); yield(); continue; }
    int want = avail < (int)sizeof(buf) ? avail : (int)sizeof(buf);
    int n = stream->read(buf, want);
    if (n <= 0) { delay(10); continue; }
    if (!_shaUpdate(&ctx, buf, n)) { ok = false; break; }
    if (Update.write(buf, n) != (size_t)n) { ok = false; break; }
    total += n;
    yield();
  }
  http.end();

  uint8_t digest[32];
  bool shaOk = ok && _shaFinish(&ctx, digest);
  mbedtls_sha256_free(&ctx);

  if (!shaOk || total != sizeBytes || !_hex32(digest).equalsIgnoreCase(shaHex)) {
    Serial.println("OTA: sha/size mismatch, abort");
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    Serial.println("OTA: Update.end failed");
    return false;
  }
  Serial.printf("OTA: wrote %d bytes, sha OK\n", total);
  return true;
}

// 新固件启动成功后调用：确认本次升级有效，取消 bootloader 回滚标记
static void otaInit() {
  esp_ota_mark_app_valid_cancel_rollback();
}

// 在 WiFi 已连接时周期调用；命中更高版本则下载、校验、重启
static void otaCheck() {
  if (_otaBusy || WiFi.status() != WL_CONNECTED) return;
  if (millis() - _lastOtaCheck < OTA_CHECK_INTERVAL_MS) return;
  _lastOtaCheck = millis();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = String(gCfg.endpoint) +
    "/.cloud/database/rest/bms_firmware?select=version,version_code,url,sha256,size_bytes,target_devices&enabled=eq.true&order=version_code.desc&limit=20";
  http.begin(client, url);
  http.setTimeout(15000);
  http.addHeader("x-wb-webapp-access-key", gCfg.pkey);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("OTA check: HTTP %d\n", code);
    http.end();
    return;
  }
  String body = http.getString();
  http.end();

  std::vector<String> objs = _jsonObjects(body);
  for (size_t i = 0; i < objs.size(); i++) {
    int remoteCode = _jsonInt(objs[i], "version_code");
    if (remoteCode <= FW_VERSION_CODE) break;   // 已按降序排列，后面版本更低
    String target = _jsonStr(objs[i], "target_devices");
    if (!_targetMatch(target, gCfg.deviceId)) continue;

    String fwUrl = _jsonStr(objs[i], "url");
    String sha = _jsonStr(objs[i], "sha256");
    int sizeBytes = _jsonInt(objs[i], "size_bytes");
    String ver = _jsonStr(objs[i], "version");
    if (!fwUrl.startsWith("https://") || sha.length() != 64 || sizeBytes <= 0) {
      Serial.println("OTA: skip invalid release " + ver);
      continue;
    }

    Serial.println("OTA: found " + ver + " (" + String(remoteCode) +
                   "), current " + String(FW_VERSION));
    _otaBusy = true;
    bool ok = _downloadAndFlash(fwUrl, sha, sizeBytes);
    _otaBusy = false;
    if (ok) {
      Serial.println("OTA: reboot into new firmware");
      delay(500);
      ESP.restart();
    }
    break;
  }
}
