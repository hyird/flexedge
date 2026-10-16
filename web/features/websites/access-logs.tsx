import { useState } from 'react'
import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { Link } from '@tanstack/react-router'
import { ArrowRotateLeft, Pause, Play } from '@gravity-ui/icons'
import { Segment, type DataGridColumn } from '@heroui-pro/react'
import { Button, Tooltip } from '@heroui/react'
import { formatBytes, formatDate } from '@/lib/format'
import { pageLocally, useListState } from '@/lib/list-search'
import { statusReserve, type StatusDictionary } from '@/lib/status'
import { CopyButton } from '@/components/copy-button'
import { DataTable } from '@/components/data-table'
import { FormSheet } from '@/components/form-sheet'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { EmptyState } from '@/components/page'
import { DescriptionList, Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { useLiveLogs } from '@/features/logs/use-live-logs'
import { accessLogStatus, filterAccessLogs } from './access-log-filter'
import type { AccessLog } from './access-log-schema'
import {
  accessLogMethods,
  accessLogStatusClasses,
  type AccessLogsSearch,
} from './access-log-search'
import { ClientIpTooltip } from './client-ip-tooltip'
import { formatClientLocation } from './client-location'
import { websiteAccessLogHistoryQuery, websiteAccessLogsQuery } from './data'
import { useWebsite } from './website-context'

const filterKeys = ['method', 'status_class'] as const
/** State of the live tail shown next to the mode switch. */
const liveStates = {
  paused: { label: '已暂停', tone: 'default' },
  connected: { label: '实时连接', tone: 'success' },
  connecting: { label: '正在连接', tone: 'accent' },
} as const satisfies StatusDictionary<'paused' | 'connected' | 'connecting'>
const liveStateReserve = statusReserve(liveStates)

/** Website detail “访问日志” tab. Mode, filters, page and the open request
 * are component state, so leaving the tab resets them and the URL never
 * changes. Live mode tails the website's access logs only while this tab
 * is mounted and not paused; history mode reads one page per query. */
export function AccessLogs() {
  const website = useWebsite()
  const list = useListState<AccessLogsSearch>()
  const [openLogId, setOpenLogId] = useState<string>()
  const { search } = list
  const history = search.mode === 'history'
  const [paused, setPaused] = useState(false)
  const filter = {
    keyword: list.keyword || undefined,
    method: search.method,
    status_class: search.status_class,
  }
  const live = useLiveLogs(
    websiteAccessLogsQuery(website.id),
    !history && !paused
  )
  const historyQuery = useQuery({
    ...websiteAccessLogHistoryQuery(website.id, {
      page: list.page,
      page_size: list.pageSize,
      ...filter,
    }),
    enabled: history,
    gcTime: 0,
    placeholderData: keepPreviousData,
  })
  const view = history
    ? historyQuery.data
    : pageLocally(filterAccessLogs(live.logs, filter), list.page, list.pageSize)
  const rows = view?.list
  const total = view?.total ?? 0
  const filtered =
    !!list.keyword || filterKeys.some((key) => search[key] !== undefined)
  const selected = openLogId
    ? (history ? historyQuery.data?.list : live.logs)?.find(
        (log) => log.id === openLogId
      )
    : undefined

  const columns: DataGridColumn<AccessLog>[] = [
    {
      id: 'time',
      header: '时间',
      minWidth: 160,
      cell: (log) => (
        <span className='text-[13px] tabular-nums'>
          {formatDate(log.occurred_at)}
        </span>
      ),
    },
    {
      id: 'node',
      header: '节点',
      minWidth: 120,
      cell: (log) => (
        <Link
          to='/nodes/$nodeId'
          params={{ nodeId: log.node_id }}
          title={log.node_name}
          className='link'
        >
          {log.node_name}
        </Link>
      ),
    },
    {
      id: 'request',
      header: '请求',
      isRowHeader: true,
      minWidth: 220,
      cell: (log) => {
        const url = log.host + log.target
        return (
          <Tooltip delay={300}>
            <Button
              size='sm'
              variant='ghost'
              className='h-auto w-max max-w-120 justify-start gap-2 px-1 py-0.5 text-left'
              aria-label={`查看请求 ${log.method} ${url}`}
              onPress={() => setOpenLogId(log.id)}
            >
              <span className='shrink-0 font-semibold'>{log.method}</span>
              <Mono className='min-w-0 truncate text-accent'>{url}</Mono>
            </Button>
            <Tooltip.Content className='max-w-120 break-all'>
              {log.method} {url}
            </Tooltip.Content>
          </Tooltip>
        )
      },
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 70,
      cell: (log) => <StatusChip {...accessLogStatus(log.status_code)} />,
    },
    {
      id: 'client',
      header: 'IP',
      minWidth: 130,
      cell: (log) => (
        <ClientIpTooltip
          asn={log.client_ip_asn}
          asName={log.client_ip_as_name}
          label={log.client_ip || '—'}
          className='font-mono text-[13px] tabular-nums'
        />
      ),
    },
    {
      id: 'location',
      header: 'IP 归属',
      minWidth: 150,
      cell: (log) => {
        const location = formatClientLocation(log.client_ip_location)
        return <span title={location}>{location}</span>
      },
    },
    {
      id: 'duration',
      header: '耗时',
      minWidth: 70,
      cell: (log) => <span className='tabular-nums'>{log.duration_ms} ms</span>,
    },
    {
      id: 'bytes',
      header: '响应',
      minWidth: 80,
      cell: (log) => (
        <span className='tabular-nums'>{formatBytes(log.response_bytes)}</span>
      ),
    },
  ]

  const liveState = paused
    ? liveStates.paused
    : live.connected
      ? liveStates.connected
      : liveStates.connecting

  return (
    <Section
      title='访问日志'
      className='access-logs min-h-0 flex-1 [&>.card__header]:shrink-0 [@media(max-height:700px)]:overflow-y-auto'
      contentClassName='flex min-h-0 flex-1 flex-col [@media(max-height:700px)]:flex-none'
      description={
        history
          ? '按条件查询已入库的访问记录。'
          : '实时接收新请求，浏览器最多保留最近 1000 条。'
      }
      actions={
        <Segment
          size='sm'
          aria-label='日志模式'
          selectedKey={history ? 'history' : 'live'}
          onSelectionChange={(key) => {
            setOpenLogId(undefined)
            void list.setSearch({
              mode: key === 'history' ? 'history' : undefined,
            })
          }}
        >
          <Segment.Item id='live'>实时</Segment.Item>
          <Segment.Item id='history'>历史</Segment.Item>
        </Segment>
      }
    >
      <div className='flex min-h-0 flex-1 flex-col gap-4'>
        {!website.config.access_log_enabled && (
          <p className='text-[13px] text-warning'>
            此网站未启用访问日志，节点不会上报新的请求记录。
          </p>
        )}
        <ListToolbar
          search={{
            value: list.keyword,
            onChange: (q) => void list.setSearch({ q }),
            placeholder: '搜索域名、路径、IP 或节点',
          }}
          filters={
            <>
              <FilterSelect
                label='方法'
                value={search.method ?? ''}
                onChange={(value) =>
                  void list.setSearch({
                    method: (value || undefined) as AccessLogsSearch['method'],
                  })
                }
                options={accessLogMethods.map((method) => ({
                  id: method,
                  label: method,
                }))}
              />
              <FilterSelect
                label='状态码'
                value={search.status_class ?? ''}
                onChange={(value) =>
                  void list.setSearch({
                    status_class: (value ||
                      undefined) as AccessLogsSearch['status_class'],
                  })
                }
                options={accessLogStatusClasses.map((value) => ({
                  id: value,
                  label: value,
                }))}
              />
            </>
          }
          hasFilters={filtered}
          onClearFilters={() => void list.clearFilters([...filterKeys])}
          onRefresh={history ? () => historyQuery.refetch() : undefined}
          isRefreshing={history && historyQuery.isFetching}
          actions={
            history ? undefined : (
              <>
                <StatusChip {...liveState} reserve={liveStateReserve} />
                <Tooltip delay={300}>
                  <Button
                    size='sm'
                    variant='tertiary'
                    isIconOnly
                    aria-label={paused ? '继续' : '暂停'}
                    onPress={() => setPaused((value) => !value)}
                  >
                    {paused ? <Play /> : <Pause />}
                  </Button>
                  <Tooltip.Content>{paused ? '继续' : '暂停'}</Tooltip.Content>
                </Tooltip>
                <Tooltip delay={300}>
                  <Button
                    size='sm'
                    variant='tertiary'
                    isIconOnly
                    aria-label='重新连接'
                    onPress={() => {
                      if (paused) setPaused(false)
                      else live.reconnect()
                    }}
                  >
                    <ArrowRotateLeft />
                  </Button>
                  <Tooltip.Content>重新连接</Tooltip.Content>
                </Tooltip>
              </>
            )
          }
        />
        <DataTable
          fit
          label={history ? '历史访问日志' : '实时访问日志'}
          variant='secondary'
          columns={columns}
          data={rows}
          isLoading={history ? historyQuery.isPending : live.loading}
          error={
            history
              ? historyQuery.isError
                ? historyQuery.error
                : undefined
              : live.error
                ? new Error(live.error)
                : undefined
          }
          onRetry={() =>
            history ? void historyQuery.refetch() : live.reconnect()
          }
          minWidth={1020}
          empty={
            filtered
              ? {
                  title: '没有匹配的访问记录',
                  description: '调整搜索词或筛选条件后重试。',
                }
              : history
                ? {
                    title: '暂无历史日志',
                    description: '启用访问日志后，节点上报的请求会保存在这里。',
                  }
                : {
                    title: paused ? '实时日志已暂停' : '等待新的请求',
                    description: paused
                      ? '继续后接收新的访问记录。'
                      : '有请求到达边缘节点时会立即出现在这里。',
                  }
          }
          footer={
            <TablePagination
              page={list.page}
              pageSize={list.pageSize}
              total={total}
              onPageChange={(page) => void list.setPage(page)}
              onPageSizeChange={(size) => void list.setPageSize(size)}
            />
          }
        />
      </div>
      <FormSheet
        isOpen={!!openLogId}
        onClose={() => setOpenLogId(undefined)}
        title='请求详情'
        size='lg'
      >
        {selected ? (
          <AccessLogDetail log={selected} />
        ) : (
            history
              ? historyQuery.isPending
              : !live.connected && !live.logs.length
          ) ? null : (
          <EmptyState
            size='sm'
            title='请求不在当前列表中'
            description={
              history
                ? '该记录不在当前页或筛选结果中。'
                : '实时日志只保留最近的记录，该请求可能已被移出。'
            }
          />
        )}
      </FormSheet>
    </Section>
  )
}

function AccessLogDetail({ log }: { log: AccessLog }) {
  const url = `${log.host}${log.target}`
  return (
    <div className='flex flex-col gap-5'>
      <div className='flex items-start gap-2 rounded-xl bg-surface-secondary p-3'>
        <span className='shrink-0 text-xs font-semibold'>{log.method}</span>
        <Mono className='min-w-0 flex-1 break-all'>{url}</Mono>
        <CopyButton value={url} label='复制地址' />
      </div>
      <DescriptionList
        columns={3}
        items={[
          { label: '时间', value: formatDate(log.occurred_at) },
          {
            label: '状态码',
            value: <StatusChip {...accessLogStatus(log.status_code)} />,
          },
          { label: '协议', value: log.protocol },
          {
            label: '节点',
            value: (
              <Link
                to='/nodes/$nodeId'
                params={{ nodeId: log.node_id }}
                className='link'
              >
                {log.node_name}
              </Link>
            ),
          },
          {
            label: 'IP',
            value: (
              <ClientIpTooltip
                asn={log.client_ip_asn}
                asName={log.client_ip_as_name}
                label={log.client_ip || '—'}
                className='font-mono text-[13px] tabular-nums'
              />
            ),
          },
          {
            label: 'IP 归属',
            value: formatClientLocation(log.client_ip_location),
          },
          { label: '耗时', value: `${log.duration_ms} ms` },
          { label: '请求流量', value: formatBytes(log.request_bytes) },
          { label: '响应流量', value: formatBytes(log.response_bytes) },
          {
            label: 'TLS 指纹',
            value: log.tls_fingerprint ? (
              <Mono className='break-all'>{log.tls_fingerprint}</Mono>
            ) : (
              '—'
            ),
            wide: true,
          },
        ]}
      />
      {log.request_body_truncated && (
        <p className='text-[13px] text-warning'>请求体超过记录上限，已截断。</p>
      )}
      {(
        [
          ['User-Agent', log.user_agent],
          ['Referer', log.referer],
          ['查询参数', log.query_string],
          ['Cookie', log.cookies],
          ['请求头', log.request_headers],
          ['请求体', log.request_body],
          ['响应头', log.response_headers],
        ] as const
      )
        .filter(([, value]) => value)
        .map(([label, value]) => (
          <div key={label} className='flex flex-col gap-1.5'>
            <div className='flex items-center justify-between gap-2'>
              <h4 className='text-xs font-medium text-muted'>{label}</h4>
              <CopyButton value={value!} />
            </div>
            <pre className='max-h-64 overflow-auto rounded-xl bg-surface-secondary p-3 font-mono text-xs break-all whitespace-pre-wrap'>
              {value}
            </pre>
          </div>
        ))}
    </div>
  )
}
