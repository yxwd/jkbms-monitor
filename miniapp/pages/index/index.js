const { cloud } = require('../../utils/cloud')

Page({
  data: {
    loading: true,
    error: '',
    reading: null,
    cells: [],
    updatedAt: '',
    socWidth: 0,
    alarmClass: 'ok',
    deviceId: '',
    appVersion: getApp().globalData.appVersion
  },

  onLoad() {
    this.setData({ deviceId: getApp().globalData.deviceId })
    this.load()
  },

  onPullDownRefresh() {
    this.load(() => wx.stopPullDownRefresh())
  },

  async load(done) {
    this.setData({ loading: true, error: '' })
    try {
      const { data, error } = await cloud.database
        .from('bms_readings')
        .select('*')
        .order('ts', { ascending: false })
        .limit(1)
        .maybeSingle()

      if (error) throw new Error(error.message || '读取失败')
      if (!data) {
        this.setData({ loading: false, error: '还没有收到设备上报的数据', reading: null })
        if (done) done()
        return
      }

      const rawCells = Array.isArray(data.cells) ? data.cells : []
      const values = rawCells.map(v => Number(v) || 0)
      const active = values.filter(v => v > 0)
      const min = active.length ? Math.min.apply(null, active) : 0
      const max = active.length ? Math.max.apply(null, active) : 0
      const cells = values.map((v, i) => ({
        no: i + 1,
        v: v.toFixed(3),
        cls: v > 0 && v === min ? ' cell-min' : (v > 0 && v === max ? ' cell-max' : '')
      }))

      // 空值兜底：个别字段缺失时显示 --，避免出现 "null"
      const disp = Object.assign({}, data)
      const numFields = ['protocol', 'soc', 'soh', 'total_voltage', 'current', 'power',
        'capacity_remain', 'nominal_capacity', 'cycles', 'temp1', 'temp2', 'mos_temp', 'balancing']
      numFields.forEach(k => {
        if (disp[k] === null || disp[k] === undefined || disp[k] === '') disp[k] = '--'
      })
      disp.alarm_text = (disp.alarm_text && disp.alarm_text !== 'null') ? disp.alarm_text : '正常'

      this.setData({
        loading: false,
        error: '',
        reading: disp,
        cells,
        socWidth: Number(data.soc) || 0,
        alarmClass: Number(data.alarms) > 0 ? 'danger' : 'ok',
        updatedAt: String(data.ts || '').replace('T', ' ').slice(0, 19)
      })
    } catch (e) {
      this.setData({ loading: false, error: e.message || '加载失败' })
    }
    if (done) done()
  },

  goHistory() {
    wx.navigateTo({ url: '/pages/history/history' })
  },

  goProvision() {
    wx.navigateTo({ url: '/pages/provision/provision' })
  },

  goFirmware() {
    wx.navigateTo({ url: '/pages/firmware/firmware' })
  }
})
