import { useLiveLogs } from '@/features/logs/use-live-logs'
import { formatDate } from '@/lib/format'
import { Badge } from '@/components/ui/badge'
import { ScrollArea } from '@/components/ui/scroll-area'
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetHeader,
  SheetTitle,
} from '@/components/ui/sheet'
import type { Node } from '@/features/nodes/types'

import { parseNodeLogs } from './node-log-schema'
import { LiveLogError } from '@/features/logs/live-log-error'

function nodeLogLevelClass(level: string) {
  switch (level.toLocaleLowerCase()) {
    case 'error':
    case 'fatal':
      return 'border-destructive/30 bg-destructive/10 text-destructive'
    case 'warn':
    case 'warning':
      return 'border-amber-500/30 bg-amber-500/10 text-amber-700 dark:text-amber-400'
    case 'debug':
      return 'border-slate-500/30 bg-slate-500/10 text-slate-700 dark:text-slate-300'
    default:
      return 'border-sky-500/30 bg-sky-500/10 text-sky-700 dark:text-sky-400'
  }
}

export function NodeLogSheet({
  node,
  onOpenChange,
}: {
  node: Node
  onOpenChange: (open: boolean) => void
}) {

  const { logs, connected, error } = useLiveLogs(`/api/nodes/${node.id}/logs/stream?limit=100`, parseNodeLogs)

  return (
    <Sheet open onOpenChange={onOpenChange}>
      <SheetContent className='flex w-full flex-col sm:max-w-2xl'>
        <SheetHeader className='text-start'>
          <div className='flex items-center gap-2'>
            <SheetTitle>{node.name} · 实时日志</SheetTitle>
            <Badge variant='outline' role='status'>
              {connected ? '已连接' : '正在重连'}
            </Badge>
          </div>
          <SheetDescription>打开时载入最近 100 条，随后持续接收实时日志，保留最近 1000 条并按时间倒序。</SheetDescription>
        </SheetHeader>
        <LiveLogError error={error} />
        <ScrollArea className='min-h-0 flex-1 px-4'>
          <div className='font-mono text-xs' role='region' aria-label='节点实时日志列表'>
            {logs.map((log) => (
              <div
                key={log.id}
                className='border-b px-3 py-2 last:border-b-0 hover:bg-muted/50'
              >
                <div className='flex min-w-0 items-center gap-2 text-muted-foreground'>
                  <time dateTime={log.occurred_at} className='shrink-0 tabular-nums'>
                    {formatDate(log.occurred_at)}
                  </time>
                  <Badge
                    variant='outline'
                    className={nodeLogLevelClass(log.level)}
                    aria-label={`日志等级 ${log.level}`}
                  >
                    {log.level}
                  </Badge>
                  <span className='min-w-0 truncate' title={log.category}>
                    {log.category}
                  </span>
                </div>
                <p className='mt-1 break-words text-foreground'>{log.message}</p>
              </div>
            ))}
            {!logs.length && (
              <p className='py-16 text-center text-muted-foreground'>
                等待日志事件…
              </p>
            )}
          </div>
        </ScrollArea>
      </SheetContent>
    </Sheet>
  )
}
