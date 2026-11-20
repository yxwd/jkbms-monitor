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
#define CLOUD_PUBLISHABLE_KEY "YOUR_PUBLISHABLE_KEY"
#define CLOUD_TABLE       "bms_readings"

// ===== 涂鸦智能（TuyaLink 标准 MQTT 协议，可选，默认关闭）=====
// 三元组 PID/DeviceID/DeviceSecret 通过 BLE 配网写入 NVS，不进源码。
// 这里只放开关与区域；上报 DP code 必须与涂鸦平台产品里定义的"标识符"完全一致。
#define TUYA_ENABLED_DEFAULT  0
// 区域：0=中国(m1.tuyacn.com) 1=美西(m1.tuyaus.com) 2=欧洲(m1.tuyaeu.com)
// 3=美东(m1-ueaz.tuyaus.com) 4=西欧(m1-weaz.tuyaeu.com) 5=印度(m1.tuyain.com) 6=新加坡(m1-sg.iotbing.com)
#define TUYA_REGION_DEFAULT   0
#define TUYA_MQTT_PORT        8883
#define TUYA_KEEPALIVE_S      60
// DP code（涂鸦平台产品功能点的"标识符"）—— 按此定义产品，否则上报无效
#define DP_VOLTAGE      "total_voltage"
#define DP_CURRENT      "current"
#define DP_SOC          "soc"
#define DP_POWER        "power"
#define DP_CAPACITY     "capacity"
#define DP_TEMP         "temp"
#define DP_MOS_TEMP     "mos_temp"
#define DP_CELL_MAX     "cell_max"
#define DP_CELL_MIN     "cell_min"
#define DP_CYCLES       "cycles"
#define DP_ALARM        "alarm"
// 涂鸦上报间隔（与 WorkBuddy 心跳一致，避免频繁连接）
#define TUYA_REPORT_INTERVAL_MS 60000

// ===== 固件版本 / OTA =====
// 每次改固件都要同步递增，否则设备不会认为有新版本
#define FW_VERSION        "1.3.0"
#define FW_VERSION_CODE   10300
// 每 5 分钟检查一次云端版本（复用已连接的 WiFi，不额外唤醒）
#define OTA_CHECK_INTERVAL_MS 300000

// ===== 时序（省云服务资源点：变化上报 + 定时心跳）=====
// 常规心跳：数据无变化时也每 60 秒上报一次，证明设备在线
#define REPORT_INTERVAL_MS      60000
// 数据显著变化时的最快上报间隔，防止抖动期间疯狂写库
#define CHANGE_REPORT_MIN_MS    10000
// BLE 采集间隔（读本地 BLE，不消耗云资源，保持 5 秒）
#define BLE_CMD_INTERVAL_MS     5000
// 采集连接复用：连接建立后持续读取，只有真正断开且冷却后才重连。
// 断连后至少等待这么久再重连，避免保护板被反复连接发出"滴"声。
#define BMS_RECONNECT_COOLDOWN_MS 3000
// 扫描极空板的时长
#define BLE_SCAN_SECONDS        10

// ===== 显著变化阈值（超过即触发即时上报）=====
#define SOC_DELTA_THRESHOLD       1     // SOC 变化 ≥1%
#define VOLTAGE_DELTA_THRESHOLD   0.15  // 总电压变化 ≥0.15V
#define CURRENT_DELTA_THRESHOLD   0.5   // 电流变化 ≥0.5A
#define TEMP_DELTA_THRESHOLD      2.0   // 温度1变化 ≥2℃
