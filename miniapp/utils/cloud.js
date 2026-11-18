const { createMiniProgramWorkBuddyCloud } = require('@tencent-ai/workbuddy-cloud-sdk/miniprogram')
const { createDiagnosticWx } = require('./workbuddy-cloud-diagnostics')
const publicConfig = require('./publicConfig')

const cloud = createMiniProgramWorkBuddyCloud({
  endpoint: publicConfig.endpoint,
  publishableKey: publicConfig.publishableKey,
  workBuddyOAuthClientId: publicConfig.workBuddyOAuthClientId,
  wx: createDiagnosticWx(wx)
})

module.exports = { cloud }
