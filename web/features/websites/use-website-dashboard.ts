import { useEffect, useState } from 'react'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { reconnectingEventSource } from '@/lib/event-stream'
import { queryKeys } from '@/lib/query-keys'
import { recoverStreamSession } from '@/features/auth/session-expiry'
import { registerSessionCleanup } from '@/features/auth/session-lifecycle'
import {
  parseWebsiteDashboard,
  type WebsiteDashboard,
} from './dashboard-schema'

export function useWebsiteDashboard(websiteId: string) {
  const client = useQueryClient()
  const [retry, setRetry] = useState(0)
  const [error, setError] = useState<string | null>(null)
  const query = useQuery<WebsiteDashboard>({
    queryKey: [...queryKeys.websites, websiteId, 'dashboard'],
    enabled: false,
    queryFn: () => {
      throw new Error('统计由实时事件提供')
    },
  })
  useEffect(() => {
    let active = true
    let stream: ReturnType<typeof reconnectingEventSource> | undefined
    const connect = () => {
      stream?.close()
      stream = reconnectingEventSource(
        `/api/websites/${websiteId}/dashboard/stream`,
        { idleTimeoutMs: null }
      )
      const current = stream
      current.addEventListener('dashboard', (event) => {
        if (!active || current !== stream) return
        try {
          const data = parseWebsiteDashboard(
            (event as MessageEvent<string>).data
          )
          client.setQueryData<WebsiteDashboard>(
            [...queryKeys.websites, websiteId, 'dashboard'],
            (previous) =>
              previous && JSON.stringify(previous) === JSON.stringify(data)
                ? previous
                : data
          )
          setError(null)
        } catch {
          setError('统计数据格式不正确，当前数据已保留。')
        }
      })
      current.addEventListener('error', () =>
        setError('统计连接中断，正在重新连接。')
      )
      current.addEventListener('session-expired', () => {
        setError('登录状态已失效，正在恢复会话。')
        void recoverStreamSession(client).then((valid) => {
          if (valid && active) connect()
        })
      })
    }
    connect()
    const cleanup = registerSessionCleanup(client, () => stream?.close())
    return () => {
      active = false
      cleanup()
      stream?.close()
    }
  }, [client, websiteId, retry])
  return {
    data: query.data,
    error,
    reconnect: () => setRetry((value) => value + 1),
  }
}
