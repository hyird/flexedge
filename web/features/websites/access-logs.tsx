import { useState } from 'react'
import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { getRouteApi, Link } from '@tanstack/react-router'
import { ArrowRotateLeft, Pause, Play } from '@gravity-ui/icons'
import { Segment, type DataGridColumn } from '@heroui-pro/react'
import { Button, Tooltip } from '@heroui/react'
import { formatBytes, formatDate } from '@/lib/format'
import { pageLocally, useListSearch } from '@/lib/list-search'
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
import { accessLogStatusTone, filterAccessLogs } from './access-log-filter'
import { parseAccessLogs, type AccessLog } from './access-log-schema'
import {
  accessLogMethods,
  accessLogStatusClasses,
  type AccessLogsSearch,
} from './access-log-search'
import { websiteAccessLogHistoryQuery } from './data'
import { useWebsite } from './website-context'
import type { WebsiteDetailSearch } from './website-detail-search'

const route = getRouteApi('/_authenticated/websites/$websiteId')
const filterKeys = ['method', 'status_class'] as const
const liveStateReserve = ['已暂停', '实时连接', '正在连接']

/** `/websites/$websiteId?tab=logs`. Live mode tails `access-logs/stream` only
 * while this tab is mounted and not paused; history mode takes one snapshot per
 * query (the `/history/stream` connection closes after the first frame). */
export function AccessLogs() {
  const website = useWebsite()
  const list = useListSearch(route)
  const navigate = route.useNavigate()
  const { search } = list
  const history = search.mode === 'history'
  const [paused, setPaused] = useState(false)
  const filter = {
    keyword: list.keyword || undefined,
    method: search.method,
    status_class: search.status_class,
  }
  const live = useLiveLogs(
    `/api/websites/${website.id}/access-logs/stream`,
    parseAccessLogs,
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
  const selected = search.log
    ? (history ? historyQuery.data?.list : live.logs)?.find(
        (log) => log.id === search.log
      )
    : undefined
  const openLog = (id: string | undefined) =>
    void navigate({
      search: (previous: WebsiteDetailSearch) => ({ ...previous, log: id }),
      replace: !id,
    })

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
      id: 'request',
      header: '请求',
      isRowHeader: true,
      minWidth: 300,
      cell: (log) => (
        <Button
          size='sm'
          variant='ghost'
          className='h-auto max-w-full min-w-0 justify-start gap-2 px-1 py-0.5 text-left'
          aria-label={`查看请求 ${log.method} ${log.host}${log.target}`}
          onPress={() => openLog(log.id)}
        >
          <span className='shrink-0 text-xs font-semibold'>{log.method}</span>
          <Mono truncate className='min-w-0 text-accent'>
            {log.host + log.target}
          </Mono>
        </Button>
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 80,
      cell: (log) => (
        <StatusChip
          label={String(log.status_code)}
          tone={accessLogStatusTone(log.status_code)}
        />
      ),
    },
    {
      id: 'client',
      header: '客户端',
      minWidth: 160,
      cell: (log) => (
        <div className='flex min-w-0 flex-col gap-0.5'>
          <Mono>{log.client_ip || '—'}</Mono>
          <span className='truncate text-xs text-muted'>
            {log.client_ip_location || '—'}
          </span>
        </div>
      ),
    },
    {
      id: 'node',
      header: '节点',
      minWidth: 130,
      cell: (log) => (
        <Link
          to='/nodes/$nodeId'
          params={{ nodeId: log.node_id }}
          className='link truncate'
        >
          {log.node_name}
        </Link>
      ),
    },
    {
      id: 'duration',
      header: '耗时',
      minWidth: 80,
      cell: (log) => <span className='tabular-nums'>{log.duration_ms} ms</span>,
    },
    {
      id: 'bytes',
      header: '响应',
      minWidth: 90,
      cell: (log) => (
        <span className='tabular-nums'>{formatBytes(log.response_bytes)}</span>
      ),
    },
  ]

  const liveState = paused
    ? { label: '已暂停', tone: 'default' as const }
    : live.connected
      ? { label: '实时连接', tone: 'success' as const }
      : { label: '正在连接', tone: 'accent' as const }

  return (
    <Section
      title='访问日志'
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
          onSelectionChange={(key) =>
            void list.setSearch({
              mode: key === 'history' ? 'history' : undefined,
              log: undefined,
            })
          }
        >
          <Segment.Item id='live'>实时</Segment.Item>
          <Segment.Item id='history'>历史</Segment.Item>
        </Segment>
      }
    >
      <div className='flex flex-col gap-4'>
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
                <StatusChip
                  label={liveState.label}
                  tone={liveState.tone}
                  reserve={liveStateReserve}
                />
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
          label={history ? '历史访问日志' : '实时访问日志'}
          columns={columns}
          data={rows}
          isLoading={
            history
              ? historyQuery.isPending
              : !paused && !live.connected && live.logs.length === 0
          }
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
          minWidth={1060}
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
        isOpen={!!search.log}
        onClose={() => openLog(undefined)}
        title='请求详情'
        size='lg'
        footer={
          <Button
            size='sm'
            variant='tertiary'
            onPress={() => openLog(undefined)}
          >
            关闭
          </Button>
        }
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
            value: (
              <StatusChip
                label={String(log.status_code)}
                tone={accessLogStatusTone(log.status_code)}
              />
            ),
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
          { label: '客户端 IP', value: <Mono>{log.client_ip || '—'}</Mono> },
          { label: 'IP 归属', value: log.client_ip_location || '—' },
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
