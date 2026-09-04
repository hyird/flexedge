import { formatDistanceToNowStrict } from 'date-fns'
import { zhCN } from 'date-fns/locale'

export function formatDate(value?: string) {
  if (!value) return '—'
  const date = new Date(value)
  if (Number.isNaN(date.getTime())) return value
  return new Intl.DateTimeFormat('zh-CN', {
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
  }).format(date)
}

export function fromNow(value?: string) {
  if (!value) return '从未'
  const date = new Date(value)
  if (Number.isNaN(date.getTime())) return value
  return formatDistanceToNowStrict(date, { addSuffix: true, locale: zhCN })
}

export function formatBytesPerSecond(value?: number) {
  if (value === undefined) return '—'
  const units = ['B/s', 'KB/s', 'MB/s', 'GB/s']
  let amount = value
  let index = 0
  while (amount >= 1024 && index < units.length - 1) {
    amount /= 1024
    index += 1
  }
  return `${amount.toFixed(index === 0 ? 0 : 1)} ${units[index]}`
}

export function initials(value?: string) {
  const text = value?.trim()
  if (!text) return 'FE'
  return text.slice(0, 2).toUpperCase()
}
