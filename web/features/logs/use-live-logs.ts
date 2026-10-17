import { useQuery } from '@tanstack/react-query'
import { statusReserve, type StatusDictionary } from '@/lib/status'
import { useTabPanelLoading } from '@/components/tab-panel-hold'
import type { LiveLogsQuery } from './data'
import type { LogEntry } from './log-event'

/** State of a live log stream shown in the log toolbar. */
export const logStreamStates = {
  paused: { label: '已暂停', tone: 'default' },
  live: { label: '实时连接', tone: 'success' },
  interrupted: { label: '连接中断', tone: 'danger' },
  connecting: { label: '正在连接', tone: 'accent' },
} as const satisfies StatusDictionary<
  'paused' | 'live' | 'interrupted' | 'connecting'
>

export const logStreamStateReserve = statusReserve(logStreamStates)

export function getLogStreamState({
  paused,
  error,
  connected,
}: {
  paused: boolean
  error?: unknown
  connected: boolean
}) {
  return {
    ...logStreamStates[
      paused
        ? 'paused'
        : connected
          ? 'live'
          : error
            ? 'interrupted'
            : 'connecting'
    ],
    reserve: logStreamStateReserve,
  }
}

/** View state of a live log tail (`liveLogsQuery`), open while `enabled`. */
export function useLiveLogs<T extends LogEntry>(
  query: LiveLogsQuery<T>,
  enabled = true
) {
  const result = useQuery({ ...query, enabled })
  const loading = enabled && result.isPending
  // A switch to this tab keeps the panel's height until the first batch.
  useTabPanelLoading(loading)
  return {
    logs: result.data ?? [],
    connected: enabled && result.status === 'success',
    /** Opening: the first batch has not arrived yet (nor failed). */
    loading,
    error: enabled && result.error ? result.error.message : null,
    reconnect: () => void result.refetch(),
  }
}
