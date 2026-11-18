const { cloud } = require('../../utils/cloud')

Page({
  data: {
    loading: true,
    error: '',
    list: [],
    count: 0,
    minV: 0,
    maxV: 0
  },

  onLoad() {
    this.load()
  },

  async load() {
    this.setData({ loading: true, error: '' })
    try {
      const { data, error } = await cloud.database
        .from('bms_readings')
        .select('ts, total_voltage, current, soc, power')
        .order('ts', { ascending: false })
        .limit(100)

      if (error) throw new Error(error.message || '读取失败')
      const rows = (data || []).slice().reverse()
      if (!rows.length) {
        this.setData({ loading: false, error: '暂无历史数据', list: [] })
        return
      }

      const values = rows.map(r => Number(r.total_voltage) || 0)
      const minV = Math.min.apply(null, values)
      const maxV = Math.max.apply(null, values)
      this.setData({
        loading: false,
        error: '',
        count: rows.length,
        minV: minV.toFixed(2),
        maxV: maxV.toFixed(2),
        list: rows.slice(-20).reverse().map(r => ({
          time: String(r.ts || '').replace('T', ' ').slice(5, 19),
          v: (Number(r.total_voltage) || 0).toFixed(2),
          a: (Number(r.current) || 0).toFixed(2),
          soc: (Number(r.soc) || 0) + '%',
          w: (Number(r.power) || 0).toFixed(0)
        }))
      })

      this.drawChart(values)
    } catch (e) {
      this.setData({ loading: false, error: e.message || '加载失败' })
    }
  },

  drawChart(values) {
    wx.createSelectorQuery().in(this).select('#chart').boundingClientRect(rect => {
      if (!rect) return
      const ctx = wx.createCanvasContext('chart', this)
      const W = rect.width
      const H = rect.height
      const pad = 24
      const min = Math.min.apply(null, values)
      const max = Math.max.apply(null, values)
      const span = max - min || 1

      ctx.clearRect(0, 0, W, H)
      ctx.setStrokeStyle('#e8eaed')
      ctx.setLineWidth(1)
      for (let i = 0; i <= 4; i++) {
        const y = pad + (H - 2 * pad) * i / 4
        ctx.beginPath()
        ctx.moveTo(pad, y)
        ctx.lineTo(W - pad, y)
        ctx.stroke()
      }

      ctx.setStrokeStyle('#185fa5')
      ctx.setLineWidth(2)
      ctx.beginPath()
      for (let i = 0; i < values.length; i++) {
        const x = pad + (W - 2 * pad) * (values.length === 1 ? 0 : i / (values.length - 1))
        const y = pad + (H - 2 * pad) * (1 - (values[i] - min) / span)
        if (i === 0) ctx.moveTo(x, y)
        else ctx.lineTo(x, y)
      }
      ctx.stroke()

      ctx.setFillStyle('#8a8f99')
      ctx.setFontSize(10)
      ctx.fillText(max.toFixed(2) + ' V', pad, 14)
      ctx.fillText(min.toFixed(2) + ' V', pad, H - 6)
      ctx.draw()
    }).exec()
  }
})
