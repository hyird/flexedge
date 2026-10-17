import { formatBytes } from '@/lib/format'
import type { CacheConfig } from './types'

export function cacheCapacityLabel(
  config: CacheConfig,
  kind: 'memory' | 'disk'
) {
  const percent = config[`${kind}_percent`]
  const reserve = config[`${kind}_reserved_bytes`]
  const capacity = percent
    ? `${percent}%`
    : formatBytes(config[`${kind}_bytes`])
  return capacity + (reserve ? `（预留 ${formatBytes(reserve)}）` : '')
}
