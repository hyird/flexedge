import { useState } from 'react'
import { keepPreviousData, useQuery } from '@tanstack/react-query'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Chip, Tabs } from '@heroui/react'
import { formatBytes, formatDate } from '@/lib/format'
import { Choice, Dialog, Field, Notice } from '@/components/forms'
import { ResourceList } from '@/components/resource-list'
import { useLiveLogs } from '@/features/logs/use-live-logs'
import { parseAccessLogs, type AccessLog } from './access-log-schema'
import { websiteAccessLogHistoryQuery } from './data'
import type { Website } from './types'

const methods = ['GET', 'HEAD', 'POST', 'PUT', 'PATCH', 'DELETE', 'OPTIONS']

export function AccessLogs({
  website,
  onClose,
}: {
  website: Website
  onClose: () => void
}) {
  const [mode, setMode] = useState('live')
  const [paused, setPaused] = useState(false)
  const [attempt, setAttempt] = useState(0)
  const [keyword, setKeyword] = useState('')
  const [search, setSearch] = useState('')
  const [method, setMethod] = useState('all')
  const [statusClass, setStatusClass] = useState('all')
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(20)
  const [selected, setSelected] = useState<AccessLog | null>(null)
  const live = useLiveLogs(
    `/api/websites/${website.id}/access-logs/stream${attempt ? `?retry=${attempt}` : ''}`,
    parseAccessLogs,
    mode === 'live' && !paused
  )
  const history = useQuery({
    ...websiteAccessLogHistoryQuery(website.id, {
      page,
      page_size: pageSize,
      keyword: search || undefined,
      method: method === 'all' ? undefined : method,
      status_class: statusClass === 'all' ? undefined : statusClass,
    }),
    enabled: mode === 'history',
    placeholderData: keepPreviousData,
  })
  const filtered = live.logs.filter(
    (log) =>
      (method === 'all' || log.method === method) &&
      (statusClass === 'all' ||
        String(log.status_code)[0] === statusClass[0]) &&
      (!search ||
        [
          log.host,
          log.target,
          log.node_name,
          log.client_ip,
          log.client_ip_location,
          log.protocol,
          String(log.status_code),
        ]
          .join(' ')
          .toLowerCase()
          .includes(search.toLowerCase()))
  )
  const livePage = {
    list: filtered.slice((page - 1) * pageSize, page * pageSize),
    total: filtered.length,
    page,
    page_size: pageSize,
    total_pages: Math.max(1, Math.ceil(filtered.length / pageSize)),
  }
  const liveQuery = {
    data: livePage,
    isPending: !paused && !live.connected && !live.logs.length,
    isError: !!live.error,
    error: live.error ? new Error(live.error) : null,
    isFetching: false,
  }
  const columns: DataGridColumn<AccessLog>[] = [
    {
      id: 'time',
      header: '时间',
      minWidth: 160,
      cell: (log) => (
        <span className='text-xs tabular-nums'>
          {formatDate(log.occurred_at)}
        </span>
      ),
    },
    {
      id: 'request',
      header: '请求',
      minWidth: 260,
      isRowHeader: true,
      cell: (log) => (
        <div className='flex flex-col items-start gap-1'>
          <Button
            size='sm'
            variant='ghost'
            className='h-auto justify-start p-0 text-xs'
            onPress={() => setSelected(log)}
          >
            <span className='font-medium'>{log.method}</span>
            <span className='max-w-64 truncate' title={log.host + log.target}>
              {log.host}
              {log.target}
            </span>
          </Button>
          <span className='text-xs text-muted'>{log.protocol}</span>
        </div>
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 80,
      cell: (log) => (
        <Chip
          size='sm'
          variant='soft'
          color={
            log.status_code >= 500
              ? 'danger'
              : log.status_code >= 400
                ? 'warning'
                : log.status_code >= 300
                  ? 'accent'
                  : 'success'
          }
        >
          {log.status_code}
        </Chip>
      ),
    },
    {
      id: 'client',
      header: '客户端',
      minWidth: 160,
      cell: (log) => (
        <div className='flex flex-col gap-1'>
          <code className='text-xs'>{log.client_ip || '—'}</code>
          <span className='text-xs text-muted'>
            {log.client_ip_location || ''}
          </span>
        </div>
      ),
    },
    { id: 'node', header: '节点', accessorKey: 'node_name', minWidth: 120 },
    {
      id: 'duration',
      header: '耗时',
      minWidth: 95,
      cell: (log) => <span className='tabular-nums'>{log.duration_ms} ms</span>,
    },
    {
      id: 'bytes',
      header: '响应',
      minWidth: 100,
      cell: (log) => (
        <span className='tabular-nums'>{formatBytes(log.response_bytes)}</span>
      ),
    },
    {
      id: 'cache',
      header: '缓存',
      minWidth: 110,
      cell: (log) => log.cache_status || '—',
    },
  ]
  return (
    <Dialog
      title={`${website.config.name || website.access_domain} · 访问日志`}
      onClose={onClose}
      size='lg'
    >
      <Tabs
        selectedKey={mode}
        onSelectionChange={(key) => {
          setMode(String(key))
          setPage(1)
          setSelected(null)
        }}
        variant='secondary'
        className='space-y-5'
      >
        <div className='flex flex-wrap items-center justify-between gap-3'>
          <Tabs.ListContainer>
            <Tabs.List aria-label='访问日志视图'>
              <Tabs.Tab id='live'>
                实时日志
                <Tabs.Indicator />
              </Tabs.Tab>
              <Tabs.Tab id='history'>
                历史日志
                <Tabs.Indicator />
              </Tabs.Tab>
            </Tabs.List>
          </Tabs.ListContainer>
          {mode === 'live' && (
            <div className='flex items-center gap-2'>
              <Chip
                size='sm'
                variant='soft'
                color={live.connected ? 'success' : 'default'}
              >
                {paused ? '已暂停' : live.connected ? '实时连接' : '正在连接'}
              </Chip>
              <Button
                size='sm'
                variant='secondary'
                onPress={() => setPaused((current) => !current)}
              >
                {paused ? '继续' : '暂停'}
              </Button>
              <Button
                size='sm'
                variant='tertiary'
                onPress={() => {
                  setAttempt((current) => current + 1)
                  setPaused(false)
                }}
              >
                重新连接
              </Button>
            </div>
          )}
        </div>
        <Tabs.Panel id={mode} className='space-y-5'>
          <ResourceList
            label='访问日志'
            query={mode === 'history' ? history : liveQuery}
            data={mode === 'history' ? history.data : livePage}
            columns={columns}
            page={page}
            onPageChange={setPage}
            pageSize={pageSize}
            onPageSizeChange={(size) => {
              setPageSize(size)
              setPage(1)
            }}
            onRefresh={
              mode === 'history' ? () => void history.refetch() : undefined
            }
            emptyTitle={mode === 'history' ? '暂无历史日志' : '暂无实时日志'}
            emptyDescription='符合当前筛选条件的访问记录将在此显示。'
            filters={
              <form
                className='flex flex-wrap items-end gap-2'
                onSubmit={(event) => {
                  event.preventDefault()
                  setSearch(keyword.trim())
                  setPage(1)
                }}
              >
                <div className='min-w-48 flex-1'>
                  <Field
                    label='搜索访问日志'
                    value={keyword}
                    onChange={setKeyword}
                    placeholder='域名、路径、IP 或节点'
                  />
                </div>
                <Choice
                  compact
                  label='请求方法'
                  value={method}
                  onChange={(value) => {
                    setMethod(value)
                    setPage(1)
                  }}
                  items={[
                    { id: 'all', label: '全部方法' },
                    ...methods.map((value) => ({ id: value, label: value })),
                  ]}
                />
                <Choice
                  compact
                  label='状态码'
                  value={statusClass}
                  onChange={(value) => {
                    setStatusClass(value)
                    setPage(1)
                  }}
                  items={[
                    { id: 'all', label: '全部状态码' },
                    ...['1xx', '2xx', '3xx', '4xx', '5xx'].map((value) => ({
                      id: value,
                      label: value,
                    })),
                  ]}
                />
                <Button size='sm' type='submit' variant='secondary'>
                  搜索
                </Button>
              </form>
            }
          />
          {selected && (
            <AccessLogDetail log={selected} onClose={() => setSelected(null)} />
          )}
        </Tabs.Panel>
      </Tabs>
    </Dialog>
  )
}

function AccessLogDetail({
  log,
  onClose,
}: {
  log: AccessLog
  onClose: () => void
}) {
  return (
    <section className='space-y-4 rounded-2xl bg-surface-secondary p-4'>
      <div className='flex flex-wrap items-center justify-between gap-2'>
        <h3 className='text-sm font-medium'>请求详情</h3>
        <Button size='sm' variant='tertiary' onPress={onClose}>
          收起
        </Button>
      </div>
      <code className='block text-sm break-all'>
        {log.method} {log.host}
        {log.target}
      </code>
      <dl className='grid gap-3 text-sm sm:grid-cols-2 lg:grid-cols-3'>
        {[
          ['时间', formatDate(log.occurred_at)],
          ['节点', log.node_name],
          ['协议', log.protocol],
          ['状态码', String(log.status_code)],
          ['客户端 IP', log.client_ip || '—'],
          ['IP 归属', log.client_ip_location || '—'],
          ['耗时', `${log.duration_ms} ms`],
          ['请求流量', formatBytes(log.request_bytes)],
          ['响应流量', formatBytes(log.response_bytes)],
          ['缓存状态', log.cache_status || '—'],
          ['缓存层', log.cache_layer || '—'],
          ['回源流量', formatBytes(log.origin_bytes)],
          ['缓存流量', formatBytes(log.cache_bytes)],
          ['TLS 指纹', log.tls_fingerprint || '—'],
        ].map(([label, value]) => (
          <div key={label}>
            <dt className='text-xs text-muted'>{label}</dt>
            <dd className='mt-1 break-all tabular-nums'>{value}</dd>
          </div>
        ))}
      </dl>
      {log.request_body_truncated && <Notice>请求体已截断。</Notice>}
      {[
        ['User-Agent', log.user_agent],
        ['Referer', log.referer],
        ['查询参数', log.query_string],
        ['Cookie', log.cookies],
        ['请求头', log.request_headers],
        ['请求体', log.request_body],
        ['响应头', log.response_headers],
      ]
        .filter(([, value]) => value)
        .map(([label, value]) => (
          <div key={label}>
            <h4 className='mb-2 text-xs font-medium'>{label}</h4>
            <pre className='max-h-64 overflow-auto rounded-xl bg-background p-3 text-xs break-all whitespace-pre-wrap'>
              {value}
            </pre>
          </div>
        ))}
    </section>
  )
}
