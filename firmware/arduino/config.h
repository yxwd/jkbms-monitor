#pragma once

// ===== WiFi =====
#define WIFI_SSID         "YOUR_WIFI_SSID"
#define WIFI_PASSWORD     "YOUR_WIFI_PASSWORD"

// ===== 极空 BMS =====
// 留空则自动扫描广播 0xFFE0 服务的设备；也可填成 "AA:BB:CC:DD:EE:FF" 直接连接
#define BMS_MAC           ""

// 0 = 自动识别；1 = JK02_24S；2 = JK02_32S
#define PROTOCOL_MODE     0

#define DEVICE_ID         "JK-B2A24S30P-01"

// ===== WorkBuddy 云服务 =====
#define CLOUD_ENDPOINT    "https://mp-api.app.workbuddy.host"
#define CLOUD_PUBLISHABLE_KEY "wbpk_NhJb648rbXrE58hc1lgpIr_g4hXEg0w3AfByZspUIif97FHbaVFRv5w"
#define CLOUD_TABLE       "bms_readings"

// ===== 时序（省云服务资源点：变化上报 + 定时心跳）=====
// 常规心跳：数据无变化时也每 60 秒上报一次，证明设备在线
#define REPORT_INTERVAL_MS      60000
// 数据显著变化时的最快上报间隔，防止抖动期间疯狂写库
#define CHANGE_REPORT_MIN_MS    10000
// BLE 采集间隔（读本地 BLE，不消耗云资源，保持 5 秒）
#define BLE_CMD_INTERVAL_MS     5000
// 扫描极空板的时长
#define BLE_SCAN_SECONDS        10

// ===== 显著变化阈值（超过即触发即时上报）=====
#define SOC_DELTA_THRESHOLD       1     // SOC 变化 ≥1%
#define VOLTAGE_DELTA_THRESHOLD   0.15  // 总电压变化 ≥0.15V
#define CURRENT_DELTA_THRESHOLD   0.5   // 电流变化 ≥0.5A
#define TEMP_DELTA_THRESHOLD      2.0   // 温度1变化 ≥2℃
