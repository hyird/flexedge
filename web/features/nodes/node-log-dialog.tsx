import { Button, Chip } from '@heroui/react'
import { formatDate } from '@/lib/format'
import { Dialog, Notice } from '@/components/forms'
import { useLiveLogs } from '@/features/logs/use-live-logs'
import { parseNodeLogs } from './node-log-schema'
import type { Node } from './types'

export function NodeLogDialog({
  node,
  onClose,
}: {
  node: Node
  onClose: () => void
}) {
  const { logs, connected, error, reconnect } = useLiveLogs(
    `/api/nodes/${node.id}/logs/stream?limit=100`,
    parseNodeLogs
  )
  return (
    <Dialog title={`${node.name} · 实时日志`} onClose={onClose} size='lg'>
      <div className='grid gap-3'>
        <div className='flex flex-wrap items-center gap-3'>
          <Chip
            size='sm'
            variant='soft'
            color={connected ? 'success' : 'warning'}
          >
            {connected ? '已连接' : error ? '连接中断' : '正在连接'}
          </Chip>
          <span className='text-xs text-muted'>
            最近 100 条起始记录，最多保留 1000 条
          </span>
          {!connected && (
            <Button size='sm' variant='tertiary' onPress={reconnect}>
              重新连接
            </Button>
          )}
        </div>
        {error && <Notice>{error}</Notice>}
        <div
          className='max-h-[60vh] min-h-48 overflow-auto rounded-lg bg-surface-secondary p-3 font-mono text-xs'
          role='region'
          aria-label='节点实时日志列表'
        >
          {logs.map((log) => (
            <article key={log.id} className='grid gap-1 py-2'>
              <div className='flex flex-wrap items-center gap-2 text-muted'>
                <time className='tabular-nums' dateTime={log.occurred_at}>
                  {formatDate(log.occurred_at)}
                </time>
                <Chip
                  size='sm'
                  variant='soft'
                  color={
                    /error|fatal/i.test(log.level)
                      ? 'danger'
                      : /warn/i.test(log.level)
                        ? 'warning'
                        : 'default'
                  }
                >
                  {log.level}
                </Chip>
                <span>{log.category}</span>
              </div>
              <p className='wrap-anywhere whitespace-pre-wrap'>{log.message}</p>
            </article>
          ))}
          {!logs.length && (
            <p className='py-12 text-center text-muted'>
              {connected ? '等待日志事件…' : '正在连接日志服务…'}
            </p>
          )}
        </div>
      </div>
    </Dialog>
  )
}
