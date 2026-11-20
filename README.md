# 极空保护板远程监控（JKBMS Monitor）

通过 ESP32-C3 读取极空（JKBMS）保护板的 BLE 数据，经 WiFi 上传到 WorkBuddy 云服务，并用微信小程序查看电池状态、历史曲线、设备配网和固件远程升级（OTA）。

## 功能

- **BLE 采集**：自动扫描 `JK-B2A24S30P` 等广播 0xFFE0 服务的极空保护板，支持 JK02_24S / JK02_32S 协议自动识别。
- **上云**：WiFi 连接后以 HTTPS POST 写入 `bms_readings` 表；采用「变化上报 + 60 秒心跳」节省云服务资源点。
- **小程序**：实时数据、历史记录、BLE 配网（绑定设备 MAC、设置 WiFi 密码）、固件发布管理。
- **OTA**：设备每 5 分钟查询 `bms_firmware` 版本表，匹配 `target_devices` 后下载固件、校验 SHA256、写入备用分区并自动重启；失败自动回滚。
- **多设备管理**：每台设备有唯一 `device_id`，配网时绑定保护板 MAC，避免同名 SSID / 多块板误连。

## 目录结构

```
.
├── PROTOCOL.md                  # BLE 配网协议 + OTA 接口说明
├── firmware/
│   ├── platformio/              # 推荐：PlatformIO 工程（含 OTA）
│   │   ├── platformio.ini
│   │   └── src/
│   │       ├── main.cpp
│   │       ├── config.h         # WiFi / 云 / 版本 / 上报阈值
│   │       ├── config_store.h   # NVRAM 配置存取
│   │       ├── provision.h      # BLE 配网 GATT 服务
│   │       └── ota.h            # OTA 轮询、下载、校验、回滚
│   ├── arduino/                 # Arduino IDE 版本（基础功能）
│   └── releases/
│       ├── v1.2.0-ota.bin       # 首版带 OTA 的基础固件
│       └── v1.2.0-ota.bin.sha256
└── miniapp/                     # 微信小程序源码
    ├── pages/index/             # 实时数据
    ├── pages/history/           # 历史记录
    ├── pages/provision/         # BLE 配网
    ├── pages/firmware/          # 固件发布管理
    └── utils/                   # 云服务封装、SHA256、诊断
```

## 快速开始

### 1. 编译固件

推荐 PlatformIO：

```bash
cd firmware/platformio
pio run -e esp32c3
```

产物在 `.pio/build/esp32c3/firmware.bin`。Arduino IDE 用户可直接打开 `firmware/arduino/jkbms_esp32c3.ino`。

### 2. 烧录与配网

1. 首次烧录：用串口烧录 `firmware/releases/v1.2.0-ota.bin`（或自己编译的 bin）。
2. 打开小程序 →「设备配网 / 更换绑定」→ 扫描 ESP32 的 BLE 配网服务。
3. 扫描保护板、选择要绑定的 MAC、填写 WiFi 账号密码、保存并重启。
4. 首页应出现实时电压、电流、SOC 等数据。

### 3. 导入小程序

用微信开发者工具打开 `miniapp/` 目录，先在 `project.config.json` 填入你自己的 AppID。`request` 合法域名需包含 `https://mp-api.app.workbuddy.host`。

首次编译前还需要在 `miniapp/utils/publicConfig.js` 和 `firmware/platformio/src/config.h` 填入你自己的 WorkBuddy 云应用配置（`resourceId` / `publishableKey` / `oauthClientId`）。本仓库为公开模板，不含真实凭证。

## 云端表

| 表 | 用途 | 关键字段 |
|---|---|---|
| `bms_readings` | 设备上报读数 | `device_id`, `ts`, `total_voltage`, `current`, `soc`, `cells`, `fw_version` |
| `bms_firmware` | OTA 版本注册表 | `version`, `version_code`, `url`, `sha256`, `size_bytes`, `target_devices`, `enabled` |

`target_devices` 填 `*` 表示全量，也可填逗号分隔的设备 ID 做灰度。新版本 `version_code` 必须大于设备当前版本码（当前为 `10200`）。

## 发布 OTA 版本

1. 编译新固件，得到 `firmware.bin`。
2. 把 bin 上传到**公开 HTTPS 地址**（GitHub Release 附件、对象存储公开读或自有服务器均可）。
3. 计算 SHA256 和实际字节数：

   ```bash
   sha256sum firmware.bin
   wc -c < firmware.bin
   ```

4. 小程序 →「固件远程升级」→ 填写版本号、版本码、URL、SHA256、大小、目标设备 → 发布。
5. 设备 5 分钟内自动升级。升级期间 BLE 采集暂停约 5–20 秒，请勿断电。

> 注意：WorkBuddy 云存储只提供登录用户最长 3600 秒的签名链接，不能作为 OTA 下载地址。

### 固件发布包

`firmware/releases/` 下只保留最新版的 `v1.2.6-ota.bin`（含 SHA256 校验文件），旧版本不随仓库分发。历史版本的 bin 与 SHA256 请自行在本地留存，便于回滚。

实际 OTA 下载地址在云端 `bms_firmware` 表中维护，本仓库不公开托管地址与访问码。

## 安全提示

- `miniapp/utils/publicConfig.js` 中的 `publishableKey` 按 WorkBuddy 设计可随小程序发布。本仓库为公开模板，已替换为占位符；请在自己的私有环境填入真实值。
- 云端 `bms_firmware`（OTA 版本表）的匿名 INSERT/UPDATE/DELETE 策略已收窄，仅保留匿名 SELECT 供设备轮询。发布新版本需通过有权限的后台操作写入；`bms_readings` 仍允许匿名 INSERT（设备上报）与 SELECT。
- 固件 `config.h` 中的 `WIFI_SSID` / `WIFI_PASSWORD` 是占位符；实际 WiFi 凭证通过 BLE 配网写入 NVRAM，不进源码。
- 当前 BLE 配网服务无鉴权，范围内任何人都能重配设备。量产前建议增加配网口令或首次配网后锁定。
- OTA 已做 SHA256 校验和版本号单调递增判断；仍建议使用可信 HTTPS 托管固件。

详细协议见 [PROTOCOL.md](PROTOCOL.md)。
