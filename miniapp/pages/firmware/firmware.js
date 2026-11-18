const { cloud } = require('../../utils/cloud')
const publicConfig = require('../../utils/publicConfig')
const { sha256ArrayBuffer } = require('../../utils/sha256')

Page({
  data: {
    loading: true,
    error: '',
    versions: [],
    computing: false,
    fileName: '',
    adminToken: '',
    needToken: false,
    form: {
      version: '',
      versionCode: '',
      url: '',
      sha256: '',
      sizeBytes: '',
      targetDevices: '*',
      notes: '',
      mandatory: false,
      enabled: true
    }
  },

  onLoad() {
    this.setData({ needToken: !!publicConfig.firmwareAdminToken })
    this.load()
  },

  onPullDownRefresh() {
    this.load(() => wx.stopPullDownRefresh())
  },

  async load(done) {
    this.setData({ loading: true, error: '' })
    try {
      const { data, error } = await cloud.database
        .from('bms_firmware')
        .select('*')
        .order('version_code', { ascending: false })
        .limit(30)
      if (error) throw new Error(error.message || '读取失败')
      const versions = (data || []).map(v => Object.assign({}, v, {
        timeText: String(v.created_at || '').replace('T', ' ').slice(0, 19),
        targetText: v.target_devices || '*'
      }))
      this.setData({ loading: false, versions })
    } catch (e) {
      this.setData({ loading: false, error: e.message || '加载失败' })
    }
    if (done) done()
  },

  onField(e) {
    const key = e.currentTarget.dataset.key
    const patch = {}
    patch[key === 'adminToken' ? 'adminToken' : 'form.' + key] = e.detail.value
    this.setData(patch)
  },

  onSwitch(e) {
    const key = e.currentTarget.dataset.key
    const patch = {}
    patch['form.' + key] = e.detail.value
    this.setData(patch)
  },

  chooseFile() {
    wx.chooseMessageFile({
      count: 1,
      type: 'file',
      extension: ['bin'],
      success: (res) => {
        const f = res.tempFiles && res.tempFiles[0]
        if (!f) return
        this.setData({ computing: true })
        try {
          const fm = wx.getFileSystemManager()
          const data = fm.readFileSync(f.path)
          const hash = sha256ArrayBuffer(data)
          this.setData({
            computing: false,
            fileName: f.name,
            'form.sha256': hash,
            'form.sizeBytes': String(f.size)
          })
          wx.showToast({ title: '校验值已生成', icon: 'success' })
        } catch (err) {
          this.setData({ computing: false })
          wx.showToast({ title: '读取文件失败', icon: 'none' })
        }
      },
      fail: () => {}
    })
  },

  checkToken() {
    if (!publicConfig.firmwareAdminToken) return true
    if (this.data.adminToken === publicConfig.firmwareAdminToken) return true
    wx.showToast({ title: '管理口令不正确', icon: 'none' })
    return false
  },

  async publish() {
    if (!this.checkToken()) return
    const f = this.data.form
    if (!f.version || !f.versionCode || !f.url || !f.sha256 || !f.sizeBytes) {
      wx.showToast({ title: '请填完整版本、版本码、地址、校验值和大小', icon: 'none' })
      return
    }
    if (!/^https:\/\//.test(f.url)) {
      wx.showToast({ title: '固件地址必须是 https:// 开头', icon: 'none' })
      return
    }
    if (!/^[0-9a-fA-F]{64}$/.test(f.sha256)) {
      wx.showToast({ title: '校验值必须是 64 位十六进制', icon: 'none' })
      return
    }
    const code = Number(f.versionCode)
    const size = Number(f.sizeBytes)
    if (!Number.isInteger(code) || code <= 0 || !Number.isInteger(size) || size <= 0) {
      wx.showToast({ title: '版本码和大小必须是正整数', icon: 'none' })
      return
    }

    wx.showLoading({ title: '发布中' })
    try {
      const { data, error } = await cloud.database
        .from('bms_firmware')
        .insert({
          version: f.version,
          version_code: code,
          url: f.url,
          sha256: f.sha256.toLowerCase(),
          size_bytes: size,
          target_devices: (f.targetDevices || '').trim() || '*',
          mandatory: !!f.mandatory,
          enabled: !!f.enabled,
          notes: f.notes || null
        })
        .select()
      wx.hideLoading()
      if (error) throw new Error(error.message || '发布失败')
      if (!data || !data.length) throw new Error('写入被拒绝，请检查权限')
      wx.showToast({ title: '已发布', icon: 'success' })
      this.setData({
        fileName: '',
        'form.version': '',
        'form.versionCode': String(code + 1),
        'form.url': '',
        'form.sha256': '',
        'form.sizeBytes': '',
        'form.notes': ''
      })
      this.load()
    } catch (e) {
      wx.hideLoading()
      wx.showToast({ title: e.message || '发布失败', icon: 'none' })
    }
  },

  async toggle(e) {
    if (!this.checkToken()) return
    const id = e.currentTarget.dataset.id
    const enabled = e.currentTarget.dataset.enabled === true || e.currentTarget.dataset.enabled === 'true'
    try {
      const { data, error } = await cloud.database
        .from('bms_firmware')
        .update({ enabled: !enabled })
        .eq('id', id)
        .select()
      if (error) throw new Error(error.message || '操作失败')
      if (!data || !data.length) throw new Error('没有权限修改')
      this.load()
    } catch (err) {
      wx.showToast({ title: err.message || '操作失败', icon: 'none' })
    }
  },

  async remove(e) {
    if (!this.checkToken()) return
    const id = e.currentTarget.dataset.id
    const ok = await new Promise(resolve => {
      wx.showModal({
        title: '删除版本',
        content: '确认删除这条发布记录？',
        success: r => resolve(!!r.confirm)
      })
    })
    if (!ok) return
    try {
      const { data, error } = await cloud.database
        .from('bms_firmware')
        .delete()
        .eq('id', id)
        .select()
      if (error) throw new Error(error.message || '删除失败')
      if (!data || !data.length) throw new Error('没有权限删除')
      this.load()
    } catch (err) {
      wx.showToast({ title: err.message || '删除失败', icon: 'none' })
    }
  },

  copyUrl(e) {
    wx.setClipboardData({ data: e.currentTarget.dataset.url || '' })
  }
})
