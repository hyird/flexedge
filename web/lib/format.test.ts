import { describe, expect, it } from 'vitest'
import { apiErrorMessage } from './api'
import { formatBytesPerSecond, formatDate, initials } from './format'

describe('format helpers', () => {
  it('formats transfer rates with binary units', () => {
    expect(formatBytesPerSecond(0)).toBe('0 B/s')
    expect(formatBytesPerSecond(1536)).toBe('1.5 KB/s')
    expect(formatBytesPerSecond(undefined)).toBe('—')
  })

  it('keeps invalid timestamps visible for diagnosis', () => {
    expect(formatDate()).toBe('—')
    expect(formatDate('invalid-timestamp')).toBe('invalid-timestamp')
  })

  it('derives compact avatar initials', () => {
    expect(initials(' flexedge ')).toBe('FL')
    expect(initials()).toBe('FE')
  })
})

describe('API errors', () => {
  it('preserves useful runtime error messages', () => {
    expect(apiErrorMessage(new Error('连接已关闭'))).toBe('连接已关闭')
    expect(apiErrorMessage(null)).toBe('操作失败，请稍后重试')
  })
})
