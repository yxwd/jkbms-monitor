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

用微信开发者工具打开 `miniapp/` 目录。AppID 为 `wx36c7cb7054beebe1`（可在 `project.config.json` 修改）。`request` 合法域名需包含 `https://mp-api.app.workbuddy.host`。

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

### 当前已发布版本

- `v1.2.0`，版本码 `10200`
- 下载地址：`https://jkbms-monitor.app.workbuddy.host/firmware.bin?sharecode=V477GMRG3H`
- SHA256：`48e8df6b86f976c96c96bdae756203af4a5aedff67c17d078ba73954d7e0dd35`
- 大小：`1184096` 字节

该地址由 WorkBuddy 静态托管提供，已写入云端 `bms_firmware` 表。后续新版本替换托管目录下的 `firmware.bin` 并在云端新增版本记录即可。

## 安全提示

- `miniapp/utils/publicConfig.js` 中的 `publishableKey` 按 WorkBuddy 设计可随小程序发布，但它允许匿名读写云端表。生产环境应接入登录并收窄 RLS 策略。
- 固件 `config.h` 中的 `WIFI_SSID` / `WIFI_PASSWORD` 是占位符；实际 WiFi 凭证通过 BLE 配网写入 NVRAM，不进源码。
- 当前 BLE 配网服务无鉴权，范围内任何人都能重配设备。量产前建议增加配网口令或首次配网后锁定。
- OTA 已做 SHA256 校验和版本号单调递增判断；仍建议使用可信 HTTPS 托管固件。

详细协议见 [PROTOCOL.md](PROTOCOL.md)。
