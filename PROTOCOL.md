# 极空 BMS 监控器 — BLE 配网协议 v1

## 角色
ESP32-C3 同时扮演两个 BLE 角色：
- **Peripheral（配网服务）**：一直广播，供小程序连接后配置 WiFi / 绑定保护板
- **Central（采集）**：连接已绑定的极空保护板，读取电池数据

## GATT 服务

Service UUID：`0000FFF0-0000-1000-8000-00805F9B34FB`

| 特征 | UUID | 属性 | 说明 |
|------|------|------|------|
| 写 | `FFF1` | write, writeNoRsp | 小程序 → 设备，发送命令（可能分片） |
| 通知 | `FFF2` | notify | 设备 → 小程序，返回响应（可能分片） |

广播名：`JKBMS-XXXX`（XXXX 为芯片 MAC 后 4 位，便于多设备区分）

## 分片协议

BLE 单包有效载荷 ≤ 20 字节。所有命令和响应都按以下规则分片：

- 每帧 = 1 字节头 + ≤19 字节数据
- 头字节：`bit7=1` 表示首帧，`bit6=1` 表示末帧，`bit5..0` = 序列号（0~63）
- 单片消息：头 = `0xC0`（既首既末），序号 0
- 多片：首帧 `0x80|seq`，中间帧 `0x00|seq`，末帧 `0x40|seq`
- 接收端按序号拼接，收到末帧后处理完整消息

单次消息上限 8 KB（足够传扫描列表）。

## 命令（小程序 → 设备）

命令为 UTF-8 文本，格式 `CMD[,参数...]`，大小写敏感。

| 命令 | 参数 | 说明 |
|------|------|------|
| `PING` | 无 | 心跳，返回 `PONG` |
| `SCAN_BMS` | 无 | 扫描附近极空板，返回 `BMS_LIST,...` |
| `GET_CFG` | 无 | 读取当前配置，返回 `CFG,...` |
| `SET_WIFI` | `ssid,password` | 设置 WiFi（支持逗号用 `\,` 转义），写入 NVRAM；返回 `OK` 或 `ERR:原因` |
| `SET_BMS` | `MAC` | 绑定保护板 MAC（格式 `AA:BB:CC:DD:EE:FF`），写入 NVRAM；返回 `OK` |
| `SET_ID` | `deviceId` | 设置上报设备 ID，写入 NVRAM；返回 `OK` |
| `SET_CLOUD` | `endpoint,publishableKey,table` | 设置云服务参数，写入 NVRAM；返回 `OK` |
| `REBOOT` | 无 | 重启设备（延迟 200ms 后重启，先回 `OK`） |
| `FACTORY_RESET` | 无 | 清空所有 NVRAM 配置，返回 `OK` 并重启 |

## 响应（设备 → 小程序）

| 响应 | 格式 | 说明 |
|------|------|------|
| `PONG` | `PONG,<版本>,<ip>` | 心跳，含固件版本和当前 IP（未连 WiFi 显示 `NONE`） |
| `BMS_LIST` | `BMS_LIST,MAC\|RSSI\|NAME;MAC\|RSSI\|NAME;...` | 扫描到的极空板列表 |
| `CFG` | `CFG,<ssid>,<bmsMac>,<deviceId>,<wifiState>` | 当前配置，wifiState=`CONNECTED`/`FAILED`/`IDLE` |
| `OK` | `OK` | 操作成功 |
| `ERR` | `ERR:<原因>` | 操作失败 |

## 数据上报

与配网服务无关，仍走 HTTPS POST 到 `CLOUD_ENDPOINT/.cloud/database/rest/CLOUD_TABLE`，
header 带 `x-wb-webapp-access-key`。上报策略见 config.h（变化上报 + 心跳）。

## NVRAM 存储键（Preferences namespace "jkbms"）

| 键 | 类型 | 说明 |
|----|------|------|
| `ssid` | string | WiFi 名称 |
| `pass` | string | WiFi 密码 |
| `bmsmac` | string | 绑定的保护板 MAC |
| `devid` | string | 上报设备 ID |
| `endpoint` | string | 云服务地址 |
| `pkey` | string | 云服务 publishable key |
| `table` | string | 数据表名 |

## 工作流程

1. 设备上电 → 读 NVRAM → 若有 WiFi 配置则连接；无论是否连上，都启动配网广播
2. 小程序打开配网页 → 扫描 BLE 广播名 `JKBMS-*` 的设备 → 连接
3. （可选）点"扫描附近保护板" → 发 `SCAN_BMS` → 展示列表
4. 选择一块板 → 发 `SET_BMS,<MAC>`
5. 填 WiFi 名称密码 → 发 `SET_WIFI,<ssid>,<password>`
6. 发 `REBOOT` → 设备重启 → 连 WiFi + 连接绑定的板 → 开始上报
7. 换环境/换板：重走 2~6，或 `FACTORY_RESET` 恢复出厂

## 安全说明

- 配置服务无鉴权，仅用于个人/内网场景；商业化量产建议增加配网密码或首次配网锁定
- WiFi 密码明文存 NVRAM（ESP32 NVRAM 非加密），物理接触设备可读出；高安全场景需 encryption
- 云端 `bms_firmware` 版本表当前允许 anon 写入。发布页支持可选管理口令（`utils/publicConfig.js` 的 `firmwareAdminToken`），量产前建议接入登录并收窄 RLS

## OTA 远程升级（v1.2.0 起）

### 前置条件

- 设备必须先串口烧录 v1.2.0 及以上固件，旧版没有 OTA 逻辑
- 固件 bin 必须放在**稳定的公开 HTTPS 地址**。WorkBuddy 云存储只给登录用户提供最长 3600 秒的签名链接，不能用于 OTA。可选：GitHub Releases、对象存储公开读、自有 HTTPS 服务器
- 云端表 `bms_firmware` 已建好（RLS：设备只读，管理端可写）

### 设备端流程

1. WiFi 连接后每 5 分钟（`OTA_CHECK_INTERVAL_MS`）查询一次版本表：
   `GET {endpoint}/.cloud/database/rest/bms_firmware?select=version,version_code,url,sha256,size_bytes,target_devices&enabled=eq.true&order=version_code.desc&limit=20`
   Header 带 `x-wb-webapp-access-key`
2. 取 `version_code > FW_VERSION_CODE` 且 `target_devices` 命中本机 deviceId 的最新一条
3. HTTPS 流式下载到 OTA 备用分区，边下边算 SHA256（下载循环 yield 喂看门狗）
4. 下载完比对 SHA256 和 `size_bytes`，全部一致才 `Update.end(true)` 并重启
5. 新固件启动后调用 `esp_ota_mark_app_valid_cancel_rollback()` 确认有效，并随数据上报 `fw_version`，小程序首页可见

### 版本表字段（bms_firmware）

| 字段 | 说明 |
|------|------|
| `version` | 显示版本号，如 `1.3.0` |
| `version_code` | 递增整数，只有大于设备当前值才会升级；发布时务必递增 |
| `url` | 固件 bin 的公开 HTTPS 直链 |
| `sha256` | 固件 bin 的 SHA256，64 位十六进制 |
| `size_bytes` | **bin 文件的实际字节数**，不是 PlatformIO 输出的 `Flash used` |
| `target_devices` | `*` 表示全部设备；或逗号分隔的 deviceId 列表，用于灰度 |
| `mandatory` | 标记强制升级（当前设备端只做提示，不阻断采集） |
| `enabled` | 停用后设备不再看到该版本 |
| `notes` | 更新说明 |

v1.2.0 固件：`output/jkbms-monitor/firmware/v1.2.0-ota.bin`，大小 `1184096` 字节，
SHA256 `48e8df6b86f976c96c96bdae756203af4a5aedff67c17d078ba73954d7e0dd35`。

### 发布步骤

1. 在 PlatformIO 里递增 `config.h` 的 `FW_VERSION` 与 `FW_VERSION_CODE`，编译
2. 把 `firmware.bin` 上传到公开 HTTPS 地址，记下直链
3. 小程序首页 →「固件远程升级」→ 选本地 bin 自动算 SHA256 和大小 → 填 URL / 版本号 / 版本码 / 目标设备 → 发布
4. 设备 5 分钟内自动发现并升级；升级后首页显示新 `fw_version`，即确认成功

### 可靠性与边界

- 校验不通过、下载超时、写入失败都会 `Update.abort()`，继续运行旧固件，不会变砖
- 下载写入期间 BLE 采集和配网服务暂停约 5–20 秒，这段时间没有数据上报
- 升级过程中不要断电
- 设备端只信任 HTTPS 直链 + SHA256；`setInsecure()` 不校验证书，因此校验值必须由可信来源发布
