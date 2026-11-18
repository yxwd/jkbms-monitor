// 设备配网：BLE 连接 ESP32，扫描附近极空板，绑定 MAC，写入 WiFi
const SERVICE_UUID = '0000FFF0-0000-1000-8000-00805F9B34FB'
const WRITE_UUID = '0000FFF1-0000-1000-8000-00805F9B34FB'
const NOTIFY_UUID = '0000FFF2-0000-1000-8000-00805F9B34FB'
const APP_VERSION = getApp().globalData.appVersion

// 分片重组（与固件协议一致：1字节头 + ≤19字节数据）
function decodeFrames(chunks) {
  let msg = ''
  let nextSeq = 0
  let active = false
  const results = []
  for (const buf of chunks) {
    const bytes = new Uint8Array(buf)
    if (bytes.length < 1) continue
    const hdr = bytes[0]
    const first = !!(hdr & 0x80)
    const last = !!(hdr & 0x40)
    const seq = hdr & 0x3F
    if (first) { msg = ''; nextSeq = 0; active = true }
    if (!active) continue
    if (seq !== nextSeq) { msg = ''; active = false; continue }
    let part = ''
    for (let i = 1; i < bytes.length; i++) part += String.fromCharCode(bytes[i])
    // 按 UTF-8 解码多字节字符
    msg += decodeURIComponent(escape(part))
    nextSeq++
    if (last) { results.push(msg); msg = ''; active = false }
  }
  return results
}

// 按协议分片编码命令
function encodeFrames(text) {
  const bytes = new TextEncoder().encode(text)
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
    frame.set(chunk, 1)
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
    status: '',
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
            this.addLog('BLE: ' + (d.name || '(无名)') + ' [name=' + d.rawName +
              ', localName=' + d.localName + '] ' + d.rssi + 'dBm')
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
                  const msgs = decodeFrames([res.value])
                  if (msgs.length) {
                    msgs.forEach(m => this.onMessage(m))
                  } else {
                    // read 兜底返回的是固件保存的完整文本（不带分片头），当原始文本解析
                    this.handleRawRead(res)
                  }
                }
                wx.onBLECharacteristicValueChange(this._onBleChange)
                this.setData({ step: 2, status: '配网通道就绪', pongOk: false })
                this.addLog('通知订阅成功 · 小程序 v' + APP_VERSION)
                // 连上后主动探测设备，并回显已保存的配置（WiFi/设备ID/保护板）
                this.cmd('PING')
                setTimeout(() => this.cmd('GET_CFG'), 300)
                // 【兜底】notify 可能因 CCCD/时机问题丢失：1.5 秒后若还没收到 PONG，
                // 主动 read FFF2（固件把最新回复存为特征值），确保能拿到设备响应。
                this._pongTimer = setTimeout(() => {
                  if (!this.data.pongOk) {
                    this.addLog('未收到 notify 的 PONG，改主动读取 FFF2…')
                    this.readReply()
                  }
                }, 1500)
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

  onMessage(msg) {
    this.addLog('← ' + msg)
    if (msg.indexOf('PONG') === 0) {
      const p = msg.split(',')
      this.setData({ pongOk: true, status: '设备在线 · 固件 ' + (p[1] || '?') + ' · IP ' + (p[2] || '未连WiFi') })
      if (this._pongTimer) { clearTimeout(this._pongTimer); this._pongTimer = null }
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
    } else if (msg === 'OK') {
      wx.showToast({ title: '已保存', icon: 'success' })
    } else if (msg.indexOf('ERR') === 0) {
      wx.showToast({ title: msg, icon: 'none' })
    }
  },

  cmd(text) {
    if (!this._deviceId) return
    this.addLog('→ ' + text)
    const frames = encodeFrames(text)
    let i = 0
    const sendNext = () => {
      if (i >= frames.length) return
      wx.writeBLECharacteristicValue({
        deviceId: this._deviceId, serviceId: this._serviceId,
        characteristicId: this._writeId, value: frames[i++],
        success: () => setTimeout(sendNext, 20),
        fail: (e) => this.addLog('写入失败: ' + e.errMsg)
      })
    }
    sendNext()
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
      this.addLog('解析 read 数据失败: ' + e.errMsg)
    }
  },

  onSsidInput(e) { this.setData({ wifiSsid: e.detail.value }) },
  onPassInput(e) { this.setData({ wifiPass: e.detail.value }) },
  onIdInput(e) { this.setData({ deviceId: e.detail.value }) },

  scanBms() {
    this.setData({ bmsList: [], status: '正在扫描附近保护板（约10秒）…' })
    this.cmd('SCAN_BMS')
  },

  selectBms(e) {
    const mac = e.currentTarget.dataset.mac
    this.setData({ selectedMac: mac })
    this.addLog('已选择保护板 ' + mac)
  },

  loadCfg() { this.cmd('GET_CFG') },

  saveWifi() {
    const { wifiSsid, wifiPass } = this.data
    if (!wifiSsid) { wx.showToast({ title: '请输入 WiFi 名称', icon: 'none' }); return }
    this.cmd('SET_WIFI,' + esc(wifiSsid) + ',' + esc(wifiPass))
  },

  saveBms() {
    if (!this.data.selectedMac) { wx.showToast({ title: '请先选择保护板', icon: 'none' }); return }
    this.cmd('SET_BMS,' + esc(this.data.selectedMac))
  },

  saveId() {
    if (!this.data.deviceId) { wx.showToast({ title: '请输入设备ID', icon: 'none' }); return }
    this.cmd('SET_ID,' + esc(this.data.deviceId))
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
    if (this._pongTimer) { clearTimeout(this._pongTimer); this._pongTimer = null }
    if (this._onBleChange) { try { wx.offBLECharacteristicValueChange(this._onBleChange) } catch (e) {} this._onBleChange = null }
    if (this._deviceId) {
      wx.closeBLEConnection({ deviceId: this._deviceId })
      this._deviceId = null
    }
    wx.closeBluetoothAdapter()
    this.setData({ bleReady: false })
  }
})
