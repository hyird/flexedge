import { describe, expect, it } from 'vitest'
import { apiErrorMessage } from './api'
import {
  formatBitsPerSecond,
  formatBytes,
  formatDate,
  formatNumber,
  formatPercent,
  formatRelative,
  formatUtcDate,
  ratioOf,
} from './format'

describe('format helpers', () => {
  it('formats *_bps bandwidth as decimal bits per second', () => {
    expect(formatBitsPerSecond(999)).toBe('999 bps')
    expect(formatBitsPerSecond(3_145_728)).toBe('3.1 Mbps')
    expect(formatBitsPerSecond(Number.NaN)).toBe('—')
  })

  it('formats byte sizes with adaptive units', () => {
    expect(formatBytes(0)).toBe('0 B')
    expect(formatBytes(1536)).toBe('1.5 KB')
    expect(formatBytes(1024 * 1024)).toBe('1.0 MB')
    expect(formatBytes(undefined)).toBe('—')
  })

  it('keeps invalid timestamps visible for diagnosis', () => {
    expect(formatDate()).toBe('—')
    expect(formatDate('invalid-timestamp')).toBe('invalid-timestamp')
    expect(formatRelative('invalid-timestamp')).toBe('invalid-timestamp')
  })

  it('formats timestamps consistently through seconds', () => {
    expect(formatDate('2026-09-05T00:25:10')).toBe('2026-09-05 00:25:10')
    expect(formatDate('2026-09-05T00:25:10.869203+08')).toBe(
      '2026-09-05 00:25:10'
    )
    expect(formatUtcDate('2026-09-05T00:25:10+08:00')).toBe(
      '2026-09-04 16:25:10 UTC'
    )
  })

  it('formats relative time in both directions with a near-now threshold', () => {
    const now = Date.parse('2026-10-14T12:00:00Z')
    expect(formatRelative('2026-10-14T11:59:30Z', now)).toBe('刚刚')
    expect(formatRelative('2026-10-14T11:57:00Z', now)).toBe('3分钟前')
    expect(formatRelative('2026-10-16T12:00:00Z', now)).toBe('2天后')
  })

  it('formats counts and ratios', () => {
    expect(formatNumber(12345)).toBe('12,345')
    expect(formatNumber(123456, { compact: true })).toBe('12.3万')
    expect(formatPercent(0.124)).toBe('12.4%')
    expect(formatPercent(undefined)).toBe('—')
    expect(ratioOf(3, 0)).toBe(0)
  })
})

describe('API errors', () => {
  it('preserves useful runtime error messages', () => {
    expect(apiErrorMessage(new Error('连接已关闭'))).toBe('连接已关闭')
    expect(apiErrorMessage(null)).toBe('操作失败，请稍后重试')
  })
})
