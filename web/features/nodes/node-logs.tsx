import { useState } from 'react'
import { getRouteApi } from '@tanstack/react-router'
import { ArrowRotateLeft, Pause, Play } from '@gravity-ui/icons'
import { ListView } from '@heroui-pro/react'
import { Alert, Button } from '@heroui/react'
import { formatDate } from '@/lib/format'
import { EmptyState } from '@/components/page'
import { Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { getLogStreamState, useLiveLogs } from '@/features/logs/use-live-logs'
import { nodeLogsQuery } from './data'

const route = getRouteApi('/_authenticated/nodes/$nodeId')

/** Live node log tail: the page's live stream while this tab is open and
 * not paused; it resumes from the last cursor after an interruption. */
export function NodeLogs() {
  const { nodeId } = route.useParams()
  const [paused, setPaused] = useState(false)
  const { logs, connected, error, reconnect } = useLiveLogs(
    nodeLogsQuery(nodeId),
    !paused
  )
  const state = getLogStreamState({ paused, error, connected })
  return (
    <Section
      title='实时日志'
      contentClassName='flex flex-col gap-3'
      actions={
        <>
          <StatusChip {...state} />
          <Button
            size='sm'
            variant='tertiary'
            onPress={() => setPaused((value) => !value)}
          >
            {paused ? (
              <Play className='size-4' />
            ) : (
              <Pause className='size-4' />
            )}
            {paused ? '继续' : '暂停'}
          </Button>
          {!paused && !connected && error && (
            <Button size='sm' variant='tertiary' onPress={reconnect}>
              <ArrowRotateLeft className='size-4' />
              重新连接
            </Button>
          )}
        </>
      }
    >
      {error && !paused && (
        <Alert status='danger'>
          <Alert.Indicator />
          <Alert.Content>
            <Alert.Description>{error}</Alert.Description>
          </Alert.Content>
        </Alert>
      )}
      {logs.length ? (
        <ListView
          aria-label='节点实时日志'
          items={logs}
          virtualized
          rowHeight={40}
          variant='secondary'
          className='h-[clamp(20rem,calc(100dvh-24rem),48rem)] overflow-auto rounded-lg border border-border bg-surface-secondary font-mono text-[13px]'
        >
          {(log) => (
            <ListView.Item
              id={log.id}
              textValue={`${log.level} ${log.category} ${log.message}`}
              className='grid items-start gap-x-3 gap-y-0.5 rounded-none border-b border-separator px-3 py-2 sm:grid-cols-[11rem_4.5rem_8rem_minmax(0,1fr)]'
            >
              <time
                dateTime={log.occurred_at}
                className='text-muted tabular-nums'
              >
                {formatDate(log.occurred_at)}
              </time>
              <span
                className={
                  /error|fatal/i.test(log.level)
                    ? 'text-danger'
                    : /warn/i.test(log.level)
                      ? 'text-warning'
                      : 'text-muted'
                }
              >
                {log.level.toUpperCase()}
              </span>
              <span className='truncate text-muted'>{log.category}</span>
              <p className='min-w-0 break-words whitespace-pre-wrap'>
                {log.message}
              </p>
            </ListView.Item>
          )}
        </ListView>
      ) : (
        <EmptyState
          size='sm'
          title={
            paused
              ? '日志已暂停'
              : connected
                ? '等待日志事件'
                : '正在连接日志服务'
          }
          description={
            paused
              ? '点击“继续”恢复实时日志。'
              : '节点上报的新日志会实时出现在这里。'
          }
        />
      )}
    </Section>
  )
}
