import { useEffect, useState } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { reconnectingEventSource } from '@/lib/event-stream'
import { registerSessionCleanup } from '@/features/auth/session-lifecycle'
import { recoverStreamSession } from '@/features/auth/session-expiry'
import {
  parseWebsiteDashboard,
  type WebsiteDashboard,
} from './dashboard-schema'

export function useWebsiteDashboard(websiteId: string | undefined) {
  const client = useQueryClient()
  const [snapshot, setSnapshot] = useState<{
    websiteId: string
    data?: WebsiteDashboard
    error: string | null
  } | null>(null)

  useEffect(() => {
    if (!websiteId) return
    // The first event supplies the snapshot; later events replace it. Server
    // heartbeats have no data, so they cannot drive the client's idle watchdog.
    let active = true
    let stream: ReturnType<typeof reconnectingEventSource> | undefined
    const connect = () => {
      stream = reconnectingEventSource(`/api/websites/${websiteId}/dashboard/stream`, { idleTimeoutMs: null })
      const current = stream
      current.addEventListener('dashboard', (event) => {
      try {
        const data = parseWebsiteDashboard((event as MessageEvent<string>).data)
        setSnapshot({ websiteId, data, error: null })
      } catch {
        setSnapshot((previous) => ({
          websiteId,
          data: previous?.websiteId === websiteId ? previous.data : undefined,
          error: '网站统计格式不正确，正在等待有效数据。',
        }))
      }
      })
      current.addEventListener('error', () => {
      setSnapshot((previous) => ({
        websiteId,
        data: previous?.websiteId === websiteId ? previous.data : undefined,
        error: '网站统计连接中断，正在自动重连。',
      }))
      })
      current.addEventListener('session-expired', () => {
        setSnapshot({ websiteId, data: undefined, error: '登录状态已失效，正在恢复会话。' })
        void recoverStreamSession(client).then((valid) => {
          if (valid && active) connect()
        })
      })
    }
    connect()
    const cleanup = registerSessionCleanup(client, () => stream?.close())
    return () => { active = false; cleanup(); stream?.close() }
  }, [client, websiteId])

  return {
    data: snapshot?.websiteId === websiteId ? snapshot?.data : undefined,
    error: snapshot?.websiteId === websiteId ? snapshot?.error : null,
  }
}
