import {
  type ReactNode,
  type RefObject,
  useEffect,
  useMemo,
  useRef,
  useState,
} from 'react'
import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { Link } from '@tanstack/react-router'
import { ArrowRotateLeft, Pause, Play } from '@gravity-ui/icons'
import { Segment, type DataGridColumn } from '@heroui-pro/react'
import { Button, Table, Tooltip } from '@heroui/react'
import { formatBytes, formatDate } from '@/lib/format'
import { useListState } from '@/lib/list-search'
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
import { liveLogCapacity } from '@/features/logs/log-event'
import { useLiveLogs } from '@/features/logs/use-live-logs'
import { accessLogStatus, filterAccessLogs } from './access-log-filter'
import type { AccessLog } from './access-log-schema'
import {
  accessLogMethods,
  accessLogStatusClasses,
  type AccessLogsSearch,
} from './access-log-search'
import { cacheReason, cacheState } from './access_log_cache'
import { ClientIpTooltip } from './client-ip-tooltip'
import { formatClientLocation } from './client-location'
import { websiteAccessLogHistoryQuery, websiteAccessLogsQuery } from './data'
import { useWebsite } from './website-context'

const filterKeys = ['method', 'status_class'] as const
/** State of the live tail shown next to the mode switch. */
const liveStates = {
  paused: { label: '已暂停', tone: 'default' },
  connected: { label: '实时连接', tone: 'success' },
  interrupted: { label: '连接中断', tone: 'danger' },
  connecting: { label: '正在连接', tone: 'accent' },
} as const satisfies StatusDictionary<
  'paused' | 'connected' | 'interrupted' | 'connecting'
>
const liveStateReserve = statusReserve(liveStates)

const columnText = {
  time: (log: AccessLog) => formatDate(log.occurred_at),
  node: (log: AccessLog) => log.node_name,
  status: (log: AccessLog) => String(log.status_code),
  client: (log: AccessLog) => log.client_ip || '—',
  location: (log: AccessLog) => formatClientLocation(log.client_ip_location),
  duration: (log: AccessLog) => `${log.duration_ms} ms`,
  bytes: (log: AccessLog) => formatBytes(log.response_bytes),
}
type ContentColumn = keyof typeof columnText

/** Virtualized cells need numeric widths. Measure their real typography and
 * decorations, then account for every retained row, including offscreen rows. */
function useColumnWidths(
  rows: AccessLog[] | undefined,
  container: RefObject<HTMLDivElement | null>
) {
  const textCache = useRef(
    new WeakMap<AccessLog, Partial<Record<ContentColumn, string>>>()
  )
  const measurementCache = useRef(
    new Map<
      ContentColumn,
      { font: string; spacing: string; widths: Map<string, number> }
    >()
  )
  const [widths, setWidths] = useState<Record<ContentColumn, number>>({
    time: 190,
    node: 120,
    status: 80,
    client: 130,
    location: 150,
    duration: 90,
    bytes: 100,
  })
  useEffect(() => {
    const root = container.current
    const context = document.createElement('canvas').getContext('2d')
    if (!root || !context || !rows?.length) return
    let active = true
    const measure = () => {
      if (!active) return
      const next = {} as Record<ContentColumn, number>
      for (const id of Object.keys(columnText) as ContentColumn[]) {
        const cell = root.querySelector<HTMLElement>(`[data-log-cell="${id}"]`)
        const header = root.querySelector<HTMLElement>(
          `[data-log-header="${id}"]`
        )
        if (!cell || !header) return
        const text = document
          .createTreeWalker(cell, NodeFilter.SHOW_TEXT)
          .nextNode()?.parentElement
        if (!text) return
        const font = getComputedStyle(text)
        context.font = `${font.fontWeight} ${font.fontSize} ${font.fontFamily}`
        context.letterSpacing =
          font.letterSpacing === 'normal' ? '0px' : font.letterSpacing
        const cached = measurementCache.current.get(id)
        const previous =
          cached?.font === context.font &&
          cached.spacing === context.letterSpacing
            ? cached.widths
            : undefined
        const measured = new Map<string, number>()
        let widest = 0
        for (const row of rows) {
          let entry = textCache.current.get(row)
          if (!entry) {
            entry = {}
            textCache.current.set(row, entry)
          }
          const value = (entry[id] ??= columnText[id](row))
          const width =
            measured.get(value) ??
            previous?.get(value) ??
            context.measureText(value).width
          measured.set(value, width)
          widest = Math.max(widest, width)
        }
        // Keep only strings still present in this page or bounded live buffer.
        measurementCache.current.set(id, {
          font: context.font,
          spacing: context.letterSpacing,
          widths: measured,
        })
        const padding = getComputedStyle(cell.closest('.table__cell')!)
        next[id] = Math.ceil(
          Math.max(
            header.getBoundingClientRect().width,
            cell.getBoundingClientRect().width +
              widest -
              context.measureText(cell.textContent ?? '').width
          ) +
            parseFloat(padding.paddingInlineStart) +
            parseFloat(padding.paddingInlineEnd)
        )
      }
      setWidths((previous) =>
        (Object.keys(next) as ContentColumn[]).every(
          (id) => next[id] === previous[id]
        )
          ? previous
          : next
      )
    }
    const observer = new ResizeObserver(measure)
    observer.observe(root)
    void document.fonts.ready.then(measure)
    return () => {
      active = false
      observer.disconnect()
    }
  }, [rows, container])
  return widths
}

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
  const liveRows = useMemo(
    () =>
      filterAccessLogs(live.logs, {
        keyword: list.keyword,
        method: search.method,
        status_class: search.status_class,
      }),
    [live.logs, list.keyword, search.method, search.status_class]
  )
  const rows = history ? historyQuery.data?.list : liveRows
  const tableContainer = useRef<HTMLDivElement>(null)
  const columnWidths = useColumnWidths(rows, tableContainer)
  const filtered =
    !!list.keyword || filterKeys.some((key) => search[key] !== undefined)
  const selected = openLogId
    ? (history ? historyQuery.data?.list : live.logs)?.find(
        (log) => log.id === openLogId
      )
    : undefined

  const columns = useMemo<DataGridColumn<AccessLog>[]>(
    () => [
      {
        id: 'time',
        header: '时间',
        cell: (log) => (
          <span className='text-[13px] tabular-nums'>
            {formatDate(log.occurred_at)}
          </span>
        ),
      },
      {
        id: 'node',
        header: '节点',
        cell: (log) => (
          <Link
            to='/nodes/$nodeId'
            params={{ nodeId: log.node_id }}
            title={log.node_name}
            className='link block max-w-full truncate'
          >
            {log.node_name}
          </Link>
        ),
      },
      {
        id: 'request',
        header: '请求',
        isRowHeader: true,
        width: '1fr',
        minWidth: 300,
        maxWidth: 960,
        cell: (log) => {
          const url = log.host + log.target
          return (
            <div className='flex min-w-0 items-center gap-2'>
              <Tooltip delay={300}>
                <Button
                  size='sm'
                  variant='ghost'
                  className='h-auto w-max max-w-full min-w-0 justify-start gap-2 px-1 py-0.5 text-left'
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
              {cacheState(log) && (
                <Tooltip delay={300}>
                  <span className='inline-flex shrink-0' tabIndex={0}>
                    <StatusChip {...cacheState(log)!} />
                  </span>
                  <Tooltip.Content>{cacheReason(log)}</Tooltip.Content>
                </Tooltip>
              )}
            </div>
          )
        },
      },
      {
        id: 'status',
        header: '状态',
        cell: (log) => <StatusChip {...accessLogStatus(log.status_code)} />,
      },
      {
        id: 'client',
        header: 'IP',
        cell: (log) => (
          <ClientIpTooltip
            asn={log.client_ip_asn}
            asName={log.client_ip_as_name}
            label={log.client_ip || '—'}
            className='block max-w-full truncate font-mono text-[13px] tabular-nums'
          />
        ),
      },
      {
        id: 'location',
        header: 'IP归属',
        cell: (log) => {
          const location = formatClientLocation(log.client_ip_location)
          return (
            <span title={location} className='block max-w-full truncate'>
              {location}
            </span>
          )
        },
      },
      {
        id: 'duration',
        header: '耗时',
        cell: (log) => (
          <span className='tabular-nums'>{log.duration_ms} ms</span>
        ),
      },
      {
        id: 'bytes',
        header: '响应',
        cell: (log) => (
          <span className='tabular-nums'>
            {formatBytes(log.response_bytes)}
          </span>
        ),
      },
    ],
    []
  )
  const sizedColumns = useMemo(
    () =>
      columns.map((column) =>
        column.id === 'request'
          ? column
          : {
              ...column,
              width: columnWidths[column.id as ContentColumn],
              header: (
                <span
                  className='inline-block w-max'
                  data-log-header={column.id}
                >
                  {column.header as string}
                </span>
              ),
              cell: (log: AccessLog) => (
                <span className='inline-flex w-max' data-log-cell={column.id}>
                  {column.cell?.(log, column)}
                </span>
              ),
            }
      ),
    [columns, columnWidths]
  )

  const liveState = paused
    ? liveStates.paused
    : live.connected
      ? liveStates.connected
      : live.error !== null
        ? liveStates.interrupted
        : liveStates.connecting

  return (
    <Section
      title='访问日志'
      className='access-logs min-h-0 flex-1 [&>.card__header]:shrink-0 [@media(max-height:700px)]:overflow-y-auto'
      contentClassName='flex min-h-0 flex-1 flex-col [@media(max-height:700px)]:flex-none'
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
      <div ref={tableContainer} className='flex min-h-0 flex-1 flex-col gap-4'>
        {!!rows?.length && (
          <div
            aria-hidden
            inert
            className='pointer-events-none invisible absolute size-0 overflow-hidden'
          >
            <Table variant='secondary' className='data-grid w-max'>
              <Table.Content aria-label='访问日志列宽测量' className='w-max'>
                <Table.Header>
                  {sizedColumns
                    .filter((column) => column.id !== 'request')
                    .map((column) => (
                      <Table.Column
                        key={column.id}
                        isRowHeader={column.id === 'time'}
                      >
                        {column.header as ReactNode}
                      </Table.Column>
                    ))}
                </Table.Header>
                <Table.Body>
                  <Table.Row>
                    {sizedColumns
                      .filter((column) => column.id !== 'request')
                      .map((column) => (
                        <Table.Cell key={column.id}>
                          {column.cell?.(rows[0], column)}
                        </Table.Cell>
                      ))}
                  </Table.Row>
                </Table.Body>
              </Table.Content>
            </Table>
          </div>
        )}
        {!website.config.access_log_enabled && (
          <p className='text-[13px] text-warning'>
            此网站未启用访问日志，节点不会上报新的请求记录。
          </p>
        )}
        <ListToolbar
          search={{
            value: list.keyword,
            onChange: (q) => void list.setSearch({ q }),
            placeholder: '搜索域名、路径、IP或节点',
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
          virtualized
          label={history ? '历史访问日志' : '实时访问日志'}
          variant='secondary'
          columns={sizedColumns}
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
            history ? (
              <TablePagination
                page={list.page}
                pageSize={list.pageSize}
                total={historyQuery.data?.total ?? 0}
                onPageChange={(page) => void list.setPage(page)}
                onPageSizeChange={(size) => void list.setPageSize(size)}
              />
            ) : (
              <span className='text-[13px] text-muted tabular-nums'>
                {filtered ? `匹配 ${rows?.length ?? 0} 条，` : ''}
                {live.logs.length} / {liveLogCapacity} 条
              </span>
            )
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
            label: '缓存',
            value: cacheState(log) ? (
              <StatusChip {...cacheState(log)!} />
            ) : (
              '未上报'
            ),
          },
          { label: '缓存原因', value: cacheReason(log) || '节点未上报原因' },
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
                className='max-w-full text-left font-mono text-[13px] break-all whitespace-normal tabular-nums'
              />
            ),
          },
          {
            label: 'IP归属',
            value: formatClientLocation(log.client_ip_location),
          },
          {
            label: 'ASN',
            value:
              [log.client_ip_asn?.trim(), log.client_ip_as_name?.trim()]
                .filter(Boolean)
                .join(' ') || '—',
          },
          { label: '耗时', value: `${log.duration_ms} ms` },
          { label: '请求流量', value: formatBytes(log.request_bytes) },
          { label: '响应流量', value: formatBytes(log.response_bytes) },
          {
            label: 'TLS指纹',
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
