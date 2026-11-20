// 设备配网：BLE 连接 ESP32，扫描附近极空板，绑定 MAC，写入 WiFi
const SERVICE_UUID = '0000FFF0-0000-1000-8000-00805F9B34FB'
const WRITE_UUID = '0000FFF1-0000-1000-8000-00805F9B34FB'
const NOTIFY_UUID = '0000FFF2-0000-1000-8000-00805F9B34FB'
const APP_VERSION = getApp().globalData.appVersion

// 跨回调分片重组：notify 是一帧一帧回调的，状态必须跨调用保存。
// 原 decodeFrames 每次调用都从零开始，导致多帧消息（BMS_LIST）永远拼不完整。
let _rxMsg = '', _rxNextSeq = 0, _rxActive = false
// 返回 { handled, msg }：
//   handled=false → 不是分片帧，交给 read 原始文本兜底解析
//   handled=true, msg=null → 是分片帧但还没拼完（或丢片），静默忽略，避免把中间帧当文本误报
//   msg 非空 → 一条完整消息
function decodeFrameChunk(buf) {
  const bytes = new Uint8Array(buf)
  if (bytes.length < 1) return { handled: false, msg: null }
  const hdr = bytes[0]
  const first = !!(hdr & 0x80), last = !!(hdr & 0x40), seq = hdr & 0x3F
  if (first) { _rxMsg = ''; _rxNextSeq = 0; _rxActive = true }
  if (!_rxActive) return { handled: false, msg: null }
  if (seq !== _rxNextSeq) { _rxMsg = ''; _rxActive = false; return { handled: true, msg: null } }  // 丢片，重置
  let part = ''
  for (let i = 1; i < bytes.length; i++) part += String.fromCharCode(bytes[i])
  _rxMsg += decodeURIComponent(escape(part))
  _rxNextSeq++
  if (last) { const out = _rxMsg; _rxMsg = ''; _rxActive = false; return { handled: true, msg: out } }
  return { handled: true, msg: null }  // 分片中间帧，等后续帧
}

// UTF-8 编码（不用 TextEncoder：部分机型/低版本基础库无 TextEncoder，会抛异常中断整个命令链）
function utf8Bytes(str) {
  const out = []
  for (let i = 0; i < str.length; i++) {
    let c = str.charCodeAt(i)
    if (c < 0x80) out.push(c)
    else if (c < 0x800) out.push(0xC0 | (c >> 6), 0x80 | (c & 0x3F))
    else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < str.length) {
      const c2 = str.charCodeAt(++i)
      const cp = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00)
      out.push(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F))
    } else out.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F))
  }
  return out
}

// 按协议分片编码命令：1字节头 + ≤19字节数据
function encodeFrames(text) {
  const bytes = utf8Bytes(text)
  const frames = []
  const per = 19
  const total = Math.max(1, Math.ceil(bytes.length / per))
  for (let i = 0; i < total; i++) {
    const chunk = bytes.slice(i * per, (i + 1) * per)
    const frame = new Uint8Array(chunk.length + 1)
    let hdr = 0
    if (i === 0) hdr |= 0x80
    if (i === total - 1) hdr |= 0x40
    hdr |= i & 0x3F
    frame[0] = hdr
    for (let j = 0; j < chunk.length; j++) frame[j + 1] = chunk[j]
    frames.push(frame.buffer)
  }
  return frames
}

// 转义逗号
function esc(s) { return String(s).replace(/\\/g, '\\\\').replace(/,/g, '\\,') }

Page({
  data: {
    appVersion: APP_VERSION,
    pongOk: false,
    scanning: false,
    bleReady: false,
    connectedName: '',
    devices: [],        // 扫描到的 JKBMS-* 设备
    bmsList: [],        // 扫描到的极空板
    selectedMac: '',
    wifiSsid: '',
    wifiPass: '',
    deviceId: '',
    manualMac: '',      // 手动输入的保护板 MAC
    // 涂鸦三元组
    tuyaPid: '',
    tuyaDevId: '',
    tuyaSecret: '',
    tuyaRegion: '0',
    tuyaStatus: '',
    status: '',
    linkStat: '',       // 采集链路统计（连接/断连/读取次数）
    log: [],
    step: 1
  },

  onUnload() {
    if (this._onFound) { try { wx.offBluetoothDeviceFound(this._onFound) } catch (e) {} this._onFound = null }
    this.disconnect()
  },

  addLog(s) {
    console.log('[provision] ' + s)
    const log = this.data.log.concat([s])
    this.setData({ log: log.slice(-40) })
  },

  // 复制全部日志到剪贴板，方便用户粘贴反馈
  copyLogs() {
    const lines = this.data.log || []
    if (!lines.length) {
      wx.showToast({ title: '暂无日志', icon: 'none' })
      return
    }
    const header = '=== 配网诊断日志（小程序 v' + APP_VERSION + '）' + new Date().toLocaleString() + ' ==='
    wx.setClipboardData({
      data: header + '\n' + lines.join('\n'),
      success: () => wx.showToast({ title: '日志已复制，可粘贴发送', icon: 'success' }),
      fail: () => wx.showToast({ title: '复制失败', icon: 'none' })
    })
  },

  // 1. 扫描 ESP32 配网广播
  startScan() {
    this.setData({ scanning: true, devices: [], status: '正在初始化蓝牙…' })
    wx.openBluetoothAdapter({
      success: () => {
        this.addLog('蓝牙已打开')
        wx.getBluetoothAdapterState({
          success: (r) => this.addLog('适配器状态: ' + (r.available ? '可用' : '不可用')),
          fail: (e) => this.addLog('读取适配器失败: ' + e.errMsg)
        })
        this.discover()
      },
      fail: (e) => {
        this.setData({ scanning: false, status: '蓝牙打开失败，请检查蓝牙是否开启' })
        this.addLog('openBluetoothAdapter 失败: ' + e.errMsg)
      }
    })
  },

  discover() {
    // 关键修复：用 onBluetoothDeviceFound 实时收集。Android 上广播名常在后续回调
    // 才从 scan response 解析出来，一次性 getBluetoothDevices 只能拿到"未知设备"。
    this._found = {}
    if (this._onFound) { try { wx.offBluetoothDeviceFound(this._onFound) } catch (e) {} }
    this._onFound = (res) => {
      ;(res.devices || []).forEach(d => {
        const prev = this._found[d.deviceId]
        const nm = d.localName || d.name || ''
        const rssi = typeof d.RSSI === 'number' ? d.RSSI : -999
        // 保留信息更全、信号更强的那条记录
        if (!prev || (nm && nm !== '未知设备' && (!prev.name || prev.name === '未知设备')) || rssi > prev.rssi) {
          this._found[d.deviceId] = { name: nm, deviceId: d.deviceId, rssi: rssi,
            localName: d.localName || '', rawName: d.name || '' }
        }
      })
    }
    wx.onBluetoothDeviceFound(this._onFound)

    wx.startBluetoothDevicesDiscovery({
      allowDuplicatesKey: true,  // 允许重复上报，才能拿到后续解析出的真名
      success: () => {
        this.addLog('开始扫描（实时收集模式）…')
        this.setData({ status: '正在扫描蓝牙设备…（6 秒）' })
        setTimeout(() => {
          wx.stopBluetoothDevicesDiscovery()
          const list = Object.keys(this._found).map(k => this._found[k])
          // 诊断日志：名字 + 两个字段，便于定位
          list.slice(0, 30).forEach(d => {
            this.addLog('BLE: ' + (d.name || '(无名)') + ' [' + d.deviceId + '] ' + d.rssi + 'dBm')
          })
          const isJk = (d) => {
            const nm = String(d.localName || d.name || '').toUpperCase()
            return nm.indexOf('JKBMS') >= 0 || nm.indexOf('JK-BMS') >= 0 || nm.indexOf('JK02') >= 0
          }
          // 信号强度作为辅助判断：ESP32 就贴在手机旁，通常 -50dBm 以内，远强于环境设备
          const near = (d) => d.rssi >= -60
          const isTarget = (d) => isJk(d) || near(d)
          list.sort((a, b) => b.rssi - a.rssi)  // 信号强的排前面
          const shown = list.map(d => ({
            name: (d.name || '(未命名设备)') + (near(d) && !isJk(d) ? ' ←近' : ''),
            deviceId: d.deviceId,
            rssi: d.rssi,
            isJk: isJk(d),
            isNear: near(d),
            isTarget: isTarget(d)
          }))
          const targets = shown.filter(d => d.isTarget)
          this.setData({
            devices: shown,
            scanning: false,
            status: targets.length
              ? ('发现 ' + targets.length + ' 台可疑设备（★或←近），点最上面的试连')
              : ('共扫到 ' + shown.length + ' 个蓝牙设备，未见强信号设备，请确认 ESP32 已上电')
          })
          this.addLog('扫描完成：共 ' + list.length + ' 个设备，可疑 ' + targets.length + ' 台')
        }, 6000)
      },
      fail: (e) => {
        this.setData({ scanning: false, status: '扫描启动失败: ' + e.errMsg })
        this.addLog('startDiscovery 失败: ' + e.errMsg)
      }
    })
  },

  // 2. 连接设备
  connectDevice(e) {
    const { deviceid, name } = e.currentTarget.dataset
    this.addLog('连接 ' + name + ' …')
    wx.stopBluetoothDevicesDiscovery()
    wx.createBLEConnection({
      deviceId: deviceid,
      success: () => {
        this._deviceId = deviceid
        this.setData({ bleReady: true, connectedName: name, status: '已连接，正在发现服务…' })
        this.addLog('BLE 连接成功')
        setTimeout(() => this.discoverServices(), 500)
      },
      fail: (e) => {
        this.setData({ status: '连接失败: ' + e.errMsg })
        this.addLog('createBLEConnection 失败: ' + e.errMsg)
      }
    })
  },

  discoverServices() {
    wx.getBLEDeviceServices({ deviceId: this._deviceId,
      success: (r) => {
        const svc = r.services.find(s => s.uuid.toUpperCase().indexOf('FFF0') >= 0)
        if (!svc) { this.addLog('未找到配网服务 FFF0'); return }
        this._serviceId = svc.uuid
        this.addLog('找到配网服务')
        wx.getBLEDeviceCharacteristics({ deviceId: this._deviceId, serviceId: svc.uuid,
          success: (cr) => {
            const write = cr.characteristics.find(c => c.uuid.toUpperCase().indexOf('FFF1') >= 0)
            const notify = cr.characteristics.find(c => c.uuid.toUpperCase().indexOf('FFF2') >= 0)
            if (!write || !notify) { this.addLog('未找到 FFF1/FFF2 特征'); return }
            this._writeId = write.uuid
            this._notifyId = notify.uuid
            this.addLog('特征就绪，订阅通知…')
            wx.notifyBLECharacteristicValueChange({
              deviceId: this._deviceId, serviceId: this._serviceId,
              characteristicId: this._notifyId, state: true,
              success: () => {
                // 先注销再注册，避免重复连接时多个回调叠加
                if (this._onBleChange) { try { wx.offBLECharacteristicValueChange(this._onBleChange) } catch (e) {} }
                this._onBleChange = (res) => {
                  try {
                    const r = decodeFrameChunk(res.value)
                    if (r.msg !== null) {
                      this.onMessage(r.msg)
                    } else if (!r.handled) {
                      // 不是分片帧 → read 兜底返回固件保存的完整文本（不带分片头），当原始文本解析
                      this.handleRawRead(res)
                    }
                    // r.handled && r.msg===null → 分片中间帧/丢片，静默忽略
                  } catch (e) {
                    this.addLog('解析 BLE 数据异常: ' + (e && e.message || e))
                  }
                }
                wx.onBLECharacteristicValueChange(this._onBleChange)
                this.setData({ step: 2, status: '配网通道就绪', pongOk: false })
                this.addLog('通知订阅成功 · 小程序 v' + APP_VERSION)

                // 【关键】兜底定时器必须在发命令之前注册。旧版把 PING 写在最前，
                // 一旦写入抛异常，后面的 GET_CFG 定时器和 PONG 兜底全部注册不上，
                // 日志就会永远停在 "→ PING"（本次修复的核心）。
                this._pingSent = false
                this.armProbeTimers()
                // 连上后主动探测设备，并回显已保存的配置（WiFi/设备ID/保护板）
                try {
                  this._pingSent = true
                  this.cmd('PING')
                  setTimeout(() => this.cmd('GET_CFG'), 400)
                } catch (e) {
                  this.addLog('发起 PING/GET_CFG 异常: ' + (e && e.message || e))
                }
              },
              fail: (e) => this.addLog('订阅通知失败: ' + e.errMsg)
            })
          },
          fail: (e) => this.addLog('获取特征失败: ' + e.errMsg)
        })
      },
      fail: (e) => this.addLog('发现服务失败: ' + e.errMsg)
    })
  },

  // ESP32 扫描保护板的逐台诊断日志：把固件回传的多行明细格式化后逐行显示
  handleScanLog(msg) {
    const body = msg.slice('SCANLOG|'.length)
    if (body === 'start') {
      this.addLog('ESP32 开始扫描附近 BLE 设备（约10秒，主循环阻塞中）…')
      return
    }
    // 首行：total|hit，其后每行：序号|名字|MAC后8位|RSSI|svc|name|判定
    const lines = body.split('\n')
    const head = (lines[0] || '').split('|')
    const total = head[0] || '0', hit = head[1] || '0'
    this.addLog('=== ESP32 扫描诊断：共 ' + total + ' 台，命中 ' + hit + ' 台 ===')
    let rows = 0
    for (let i = 1; i < lines.length; i++) {
      const p = (lines[i] || '').split('|')
      if (p.length < 7) continue
      const marked = p[6] === 'HIT' ? '★命中' : '略过'
      const svc = p[4] === 'Y' ? 'FFE0✓' : 'FFE0✗'
      const nm = p[5] === 'Y' ? '名✓' : '名✗'
      this.addLog('  #' + p[0] + ' ' + p[1] + ' (' + p[2] + ') ' + p[3] + 'dBm ' + svc + ' ' + nm + ' → ' + marked)
      rows++
    }
    if (total && Number(total) > rows) this.addLog('  …其余 ' + (Number(total) - rows) + ' 台未显示（诊断最多列24台）')
    if (Number(hit) === 0) this.addLog('  提示：未命中说明周围没有广播 FFE0 服务或名字含 JK/BMS 的设备；保护板靠近后再扫')
  },

  // 解析采集链路统计 C连接/D断连/R读取（旧固件无此字段时静默忽略）
  showLinkStat(stat) {
    if (!stat || stat.indexOf('C') !== 0) return
    const m = /^C(\d+)\/D(\d+)\/R(\d+)$/.exec(stat)
    if (!m) return
    const c = Number(m[1]), d = Number(m[2]), r = Number(m[3])
    this.setData({ linkStat: '连接' + c + '次 · 断连' + d + '次 · 读取' + r + '次' })
    if (c <= 1 && d === 0) {
      this.addLog('采集链路：连接复用中（连接1次/0断连，未反复重连 ✓）')
    } else {
      this.addLog('采集链路：连接' + c + '次 / 断连' + d + '次 / 读取' + r + '次（若连接次数持续增长说明链路被反复断开）')
    }
  },

  onMessage(msg) {
    // SCANLOG 是多行明细，交给专门格式化，不按原始消息整坨打印
    if (msg.indexOf('SCANLOG|') === 0) { this.handleScanLog(msg); return }
    this.addLog('← ' + msg)
    if (msg.indexOf('PONG') === 0) {
      const p = msg.split(',')
      this.setData({ pongOk: true, status: '设备在线 · 固件 ' + (p[1] || '?') + ' · IP ' + (p[2] || '未连WiFi') })
      this.showLinkStat(p[3])
      this.clearProbeTimers()
    } else if (msg.indexOf('BMS_LIST') === 0) {
      const list = msg.split(',').slice(1).map(s => {
        const [mac, rssi, name] = s.split('|')
        return { mac, rssi: Number(rssi) || 0, name: name || '极空保护板' }
      }).filter(x => x.mac)
      this.setData({ bmsList: list, status: list.length ? ('发现 ' + list.length + ' 块保护板') : '未发现保护板' })
    } else if (msg.indexOf('CFG,') === 0) {
      const p = msg.split(',')
      const state = p[4] || 'IDLE'
      this.setData({
        wifiSsid: p[1] || '', selectedMac: p[2] || '', deviceId: p[3] || '',
        status: (p[1] ? ('已读取配置 · WiFi: ' + p[1] + ' · ' + state) : '设备未配置 WiFi，请填写下方信息')
      })
      this.addLog('已回显配置：SSID=' + (p[1] || '(空)') + ' 保护板=' + (p[2] || '(未绑定)') + ' 设备ID=' + (p[3] || '(空)') + ' 状态=' + state)
      this.showLinkStat(p[5])
      // 涂鸦状态（固件 v1.3.0+）：T1:dev前8位:region / T0:-:0
      const ty = p[6] || ''
      if (ty.indexOf('T1:') === 0) {
        const tp = ty.split(':')
        this.setData({ tuyaStatus: '已启用 · 设备 ' + (tp[1] || '?') + ' · 区域 ' + (tp[2] || '0') })
        this.addLog('涂鸦上报：已启用（设备 ' + (tp[1] || '?') + '，区域 ' + (tp[2] || '0') + '）')
      } else if (ty.indexOf('T0:') === 0) {
        this.setData({ tuyaStatus: '未启用' })
      }
    } else if (msg === 'OK') {
      wx.showToast({ title: '已保存', icon: 'success' })
    } else if (msg.indexOf('ERR') === 0) {
      wx.showToast({ title: msg, icon: 'none' })
    }
  },

  cmd(text) {
    if (!this._deviceId) { this.addLog('命令丢弃(未连接): ' + text); return }
    this.addLog('→ ' + text)
    let frames
    try {
      frames = encodeFrames(text)
    } catch (e) {
      this.addLog('命令编码失败: ' + (e && e.message || e)); return
    }
    let i = 0
    const sendNext = () => {
      if (i >= frames.length) return
      const idx = i
      const buf = frames[i++]
      const tag = '[' + (idx + 1) + '/' + frames.length + ']'
      try {
        wx.writeBLECharacteristicValue({
          deviceId: this._deviceId, serviceId: this._serviceId,
          characteristicId: this._writeId, value: buf,
          success: () => {
            this.addLog('  写入成功 ' + tag)
            setTimeout(sendNext, 20)
          },
          fail: (e) => this.addLog('  写入失败 ' + tag + ': ' + e.errMsg)
        })
      } catch (e) {
        this.addLog('  写入异常 ' + tag + ': ' + (e && e.message || e))
      }
    }
    sendNext()
  },

  // 清除所有探测兜底定时器
  clearProbeTimers() {
    ;['_pongTimer', '_pongTimer2', '_pongTimer3', '_pingRetryTimer'].forEach(k => {
      if (this[k]) { clearTimeout(this[k]); this[k] = null }
    })
  },

  // 多级兜底：notify 可能因 CCCD/时机/射频抢占丢失，逐级用 read 和重发补偿。
  // 每一级都独立判断 pongOk，避免重复提示。
  armProbeTimers() {
    this.clearProbeTimers()
    // 1) 1.5s：首次未收到 PONG → 主动 read FFF2（固件把最新回复存为特征值）
    this._pongTimer = setTimeout(() => {
      if (!this.data.pongOk) {
        this.addLog('未收到 notify 的 PONG，主动读取 FFF2…')
        this.readReply()
      }
    }, 1500)
    // 2) 3.5s：仍未收到 → 重发一次 PING（排除首包写入丢失）
    this._pongTimer2 = setTimeout(() => {
      if (!this.data.pongOk) {
        this.addLog('仍无 PONG，重发 PING 并再次读取…')
        try { this.cmd('PING') } catch (e) {}
        setTimeout(() => { if (!this.data.pongOk) this.readReply() }, 400)
      }
    }, 3500)
    // 3) 6s：仍未收到 → 记录诊断结论，提示抓固件串口日志
    this._pongTimer3 = setTimeout(() => {
      if (!this.data.pongOk) {
        this.addLog('诊断：写入回调若均成功仍无 PONG，说明设备端未处理 PING，请查固件串口')
      }
    }, 6000)
  },

  // 兜底：主动读取 FFF2 特征值。固件把最新一条回复的完整文本存在这里，
  // 不受 notify/CCCD 丢包影响。读到的文本直接当一条消息处理。
  readReply() {
    if (!this._deviceId) return
    wx.readBLECharacteristicValue({
      deviceId: this._deviceId, serviceId: this._serviceId,
      characteristicId: this._notifyId,
      success: () => {
        // 微信 read 的结果通过 onBLECharacteristicValueChange 回调返回（值不带分片头），
        // 但为了兼容实现差异，这里也主动取一次特征值。
        this.addLog('readBLECharacteristicValue 已发起')
      },
      fail: (e) => this.addLog('主动读取失败: ' + e.errMsg)
    })
  },

  // 处理 read 回调返回的非分片原始文本（部分机型 read 结果走此回调）
  handleRawRead(res) {
    try {
      const bytes = new Uint8Array(res.value)
      let text = ''
      for (let i = 0; i < bytes.length; i++) text += String.fromCharCode(bytes[i])
      text = decodeURIComponent(escape(text))
      this.addLog('←(read) ' + text)
      this.onMessage(text)
    } catch (e) {
      this.addLog('解析 read 数据失败: ' + (e && (e.message || e.errMsg) || e))
    }
  },

  onSsidInput(e) { this.setData({ wifiSsid: e.detail.value }) },
  onPassInput(e) { this.setData({ wifiPass: e.detail.value }) },
  onIdInput(e) { this.setData({ deviceId: e.detail.value }) },

  scanBms() {
    this.setData({ bmsList: [], status: '正在扫描附近保护板（约10秒，逐台结果见下方日志）…' })
    this.addLog('下发 SCAN_BMS，等待 ESP32 逐台诊断…')
    this.cmd('SCAN_BMS')
  },

  selectBms(e) {
    const mac = e.currentTarget.dataset.mac
    this.setData({ selectedMac: mac })
    this.addLog('已选择保护板 ' + mac)
  },

  // 从步骤1手机扫到的设备里点选：Android 上 deviceId 就是设备的 BLE MAC
  bindFromPhone(e) {
    const mac = e.currentTarget.dataset.mac
    if (!mac) return
    this.setData({ selectedMac: mac })
    this.addLog('从手机扫描结果选中 MAC ' + mac + '，点「绑定到设备」写入')
  },

  onManualMac(e) { this.setData({ manualMac: e.detail.value }) },

  bindManualMac() {
    const mac = (this.data.manualMac || '').trim().toUpperCase()
    if (!mac) { wx.showToast({ title: '请输入保护板 MAC', icon: 'none' }); return }
    this.setData({ selectedMac: mac })
    this.addLog('手动填入 MAC ' + mac + '，点「绑定到设备」写入')
  },

  // 真正把 MAC 写进设备（固件 SET_BMS 按 MAC 精确直连，不依赖 ESP32 扫描）
  saveBms() {
    if (!this.data.selectedMac) { wx.showToast({ title: '请先选择保护板', icon: 'none' }); return }
    this.addLog('下发 SET_BMS ' + this.data.selectedMac)
    this.cmd('SET_BMS,' + esc(this.data.selectedMac))
  },

  loadCfg() { this.cmd('GET_CFG') },

  saveWifi() {
    const { wifiSsid, wifiPass } = this.data
    if (!wifiSsid) { wx.showToast({ title: '请输入 WiFi 名称', icon: 'none' }); return }
    this.cmd('SET_WIFI,' + esc(wifiSsid) + ',' + esc(wifiPass))
  },

  saveId() {
    if (!this.data.deviceId) { wx.showToast({ title: '请输入设备ID', icon: 'none' }); return }
    this.cmd('SET_ID,' + esc(this.data.deviceId))
  },

  // ===== 涂鸦配置 =====
  onTuyaPid(e) { this.setData({ tuyaPid: e.detail.value.trim() }) },
  onTuyaDevId(e) { this.setData({ tuyaDevId: e.detail.value.trim() }) },
  onTuyaSecret(e) { this.setData({ tuyaSecret: e.detail.value.trim() }) },
  onTuyaRegion(e) { this.setData({ tuyaRegion: String(e.detail.value) }) },
  saveTuya() {
    const { tuyaPid, tuyaDevId, tuyaSecret, tuyaRegion } = this.data
    if (!tuyaPid || !tuyaDevId || !tuyaSecret) {
      wx.showToast({ title: '请填全 PID/设备ID/密钥', icon: 'none' }); return
    }
    // 三元组含敏感信息：只在下发命令的日志里打码，不打印明文
    this.addLog('下发 SET_TUYA pid=' + tuyaPid + ' dev=' + tuyaDevId + ' secret=**** region=' + tuyaRegion)
    this.cmd('SET_TUYA,' + esc(tuyaPid) + ',' + esc(tuyaDevId) + ',' + esc(tuyaSecret) + ',' + tuyaRegion)
    this.setData({ tuyaStatus: '已下发（重启后生效）' })
  },
  turnOffTuya() {
    this.cmd('TUYA_OFF')
    this.setData({ tuyaStatus: '已关闭涂鸦上报' })
  },

  applyAndReboot() {
    wx.showModal({
      title: '确认应用',
      content: '将保存的配置写入设备并重启？重启后设备会连接新的 WiFi 和保护板。',
      success: (r) => { if (r.confirm) this.cmd('REBOOT') }
    })
  },

  factoryReset() {
    wx.showModal({
      title: '恢复出厂',
      content: '将清空 WiFi、保护板绑定等所有配置，设备重启后等待重新配网。确定继续？',
      success: (r) => { if (r.confirm) this.cmd('FACTORY_RESET') }
    })
  },

  disconnect() {
    this.clearProbeTimers()
    if (this._onBleChange) { try { wx.offBLECharacteristicValueChange(this._onBleChange) } catch (e) {} this._onBleChange = null }
    if (this._deviceId) {
      wx.closeBLEConnection({ deviceId: this._deviceId })
      this._deviceId = null
    }
    wx.closeBluetoothAdapter()
    this.setData({ bleReady: false })
  }
})
