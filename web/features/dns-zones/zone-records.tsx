import {
  type CSSProperties,
  type ReactNode,
  memo,
  useCallback,
  useMemo,
  useRef,
  useState,
} from 'react'
import { Plus } from '@gravity-ui/icons'
import { Button, Chip, Tooltip, tableVariants } from '@heroui/react'
import { pageLocally, useListState } from '@/lib/list-search'
import { cn } from '@/lib/utils'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { EmptyState } from '@/components/page'
import { Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { dnsLineIndex } from './dns-lines'
import {
  recordSyncStatusLabels,
  zoneRecordRows,
  type ZoneRecordRow,
} from './dns-zone-display'
import { RecordForm } from './record-form'
import { maxRecords, recordTypes } from './records-form'
import type { DnsRecord } from './types'
import { useZoneDetail } from './use-zone-detail'

type RecordsSearch = {
  q?: string
  type?: DnsRecord['type']
  source?: ZoneRecordRow['source']
}
const filterKeys = ['type', 'source'] as const
const typeOptions = recordTypes.map((id) => ({ id, label: id }))
const sourceOptions = [
  { id: 'custom', label: '自定义' },
  { id: 'system', label: '系统' },
]
const slots = tableVariants()

/** Which inline form is open: the add panel or one record's editor. */
type OpenForm = { kind: 'add' } | { kind: 'edit'; id: string }

/** The zone's records tab, after Cloudflare's DNS records list: plain rows
 * with one 编辑 each; 添加记录 and 编辑 open a single inline form (the add
 * panel above the rows, or in place of the edited row). Rows carry no inputs,
 * so typing re-renders only the open form. Search, filters, page and the open
 * form are component state. The rows are a semantic table rather than a
 * DataGrid: a grid would take Tab and arrow keys from the form's inputs. */
export function ZoneRecords() {
  const zone = useZoneDetail().data
  const list = useListState<RecordsSearch>()
  const [open, setOpen] = useState<OpenForm | null>(null)
  const addButton = useRef<HTMLButtonElement>(null)
  const custom = zone?.config.records
  const system = zone?.runtime.projected_records
  const states = zone?.runtime.record_states
  // Live patches keep unchanged parts by reference, so rows rebuild only when
  // records or their sync states change.
  const rows = useMemo(
    () =>
      custom && system && states
        ? zoneRecordRows({
            config: { records: custom },
            runtime: { projected_records: system, record_states: states },
          })
        : [],
    [custom, system, states]
  )
  const editRecord = useCallback(
    (id: string) => setOpen({ kind: 'edit', id }),
    []
  )
  if (!zone) return null
  const closeForm = () => {
    const closed = open
    setOpen(null)
    // Focus returns to the control that opened the form.
    requestAnimationFrame(() => {
      if (closed?.kind === 'edit')
        document.getElementById(`dns-record-edit-${closed.id}`)?.focus()
      else addButton.current?.focus()
    })
  }
  const keyword = list.keyword.toLowerCase()
  const matches = rows.filter(
    ({ record, source }) =>
      (!list.search.type || record.type === list.search.type) &&
      (!list.search.source || source === list.search.source) &&
      (!keyword ||
        record.name.toLowerCase().includes(keyword) ||
        record.content.toLowerCase().includes(keyword))
  )
  // A removal can leave the current page past the end: show the last one.
  const pageCount = Math.max(1, Math.ceil(matches.length / list.pageSize))
  const page = pageLocally(
    matches,
    Math.min(list.page, pageCount),
    list.pageSize
  )
  const filtered = !!keyword || !!list.search.type || !!list.search.source
  const cloudflare = zone.dns_provider === 'cloudflare'
  const lineLabels = dnsLineIndex(zone.runtime.lines).labels
  const showLines = zone.runtime.lines.some((line) => line.code !== 'default')
  const columnCount = 6 + (showLines ? 1 : 0) + (cloudflare ? 1 : 0)
  const formRow = (record?: DnsRecord) => (
    <tr key={record?.id ?? 'new'} className={slots.row()}>
      <td
        colSpan={columnCount}
        className={cn(slots.cell(), 'h-auto whitespace-normal')}
      >
        <RecordForm
          key={zone.id + (record?.id ?? 'new')}
          zone={zone}
          record={record}
          showLines={showLines}
          onClose={closeForm}
        />
      </td>
    </tr>
  )
  return (
    <Section
      title={`DNS 记录 · ${rows.length}`}
      contentClassName='flex flex-col gap-3'
    >
      <ListToolbar
        search={{
          value: list.keyword,
          onChange: (q) => void list.setSearch({ q }),
          placeholder: '搜索名称或内容',
        }}
        filters={
          <>
            <FilterSelect
              label='类型'
              value={list.search.type ?? ''}
              onChange={(value) =>
                void list.setSearch({
                  type: (value || undefined) as typeof list.search.type,
                })
              }
              options={typeOptions}
            />
            <FilterSelect
              label='来源'
              value={list.search.source ?? ''}
              onChange={(value) =>
                void list.setSearch({
                  source: (value || undefined) as typeof list.search.source,
                })
              }
              options={sourceOptions}
            />
          </>
        }
        hasFilters={filtered}
        onClearFilters={() => void list.clearFilters(filterKeys)}
        actions={
          <Button
            ref={addButton}
            size='sm'
            isDisabled={
              open?.kind === 'add' || zone.config.records.length >= maxRecords
            }
            onPress={() => setOpen({ kind: 'add' })}
          >
            <Plus className='size-4' />
            添加记录
          </Button>
        }
      />
      <div
        className={cn(slots.base(), 'data-grid min-w-0')}
        style={{ '--data-table-min-width': '860px' } as CSSProperties}
      >
        <div className={slots.scrollContainer()}>
          <table
            aria-label='DNS 记录'
            className={cn(slots.content(), 'min-w-(--data-table-min-width)')}
          >
            <thead className={slots.header()}>
              <tr>
                <HeaderCell width={88}>类型</HeaderCell>
                <HeaderCell width={170}>名称</HeaderCell>
                <HeaderCell width={240}>内容</HeaderCell>
                {showLines && <HeaderCell width={150}>线路</HeaderCell>}
                <HeaderCell width={80} align='end'>
                  TTL
                </HeaderCell>
                {cloudflare && <HeaderCell width={90}>代理</HeaderCell>}
                <HeaderCell width={110}>同步</HeaderCell>
                <HeaderCell width={80} align='end'>
                  <span className='sr-only'>操作</span>
                </HeaderCell>
              </tr>
            </thead>
            <tbody className={slots.body()}>
              {open?.kind === 'add' && formRow()}
              {page.list.map((row) =>
                open?.kind === 'edit' && open.id === row.record.id ? (
                  formRow(row.record)
                ) : (
                  <RecordRow
                    key={`${row.source}-${row.record.id}`}
                    record={row.record}
                    source={row.source}
                    state={row.state}
                    lineLabel={
                      showLines
                        ? (lineLabels.get(row.record.line_code) ??
                          row.record.line_code)
                        : undefined
                    }
                    cloudflare={cloudflare}
                    onEdit={editRecord}
                  />
                )
              )}
              {page.list.length === 0 && open?.kind !== 'add' && (
                <tr className={slots.row()}>
                  <td
                    colSpan={columnCount}
                    className={cn(slots.cell(), 'h-auto whitespace-normal')}
                  >
                    <EmptyState
                      size='sm'
                      title={filtered ? '没有匹配的记录' : '还没有 DNS 记录'}
                      description={
                        filtered ? '调整搜索词或筛选条件后重试。' : undefined
                      }
                    />
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
        <div
          className={slots.footer({
            className: 'flex-wrap justify-between gap-3 px-3 pt-3 pb-2',
          })}
        >
          <TablePagination
            page={page.page}
            pageSize={list.pageSize}
            total={page.total}
            onPageChange={list.setPage}
            onPageSizeChange={list.setPageSize}
          />
        </div>
      </div>
    </Section>
  )
}
function HeaderCell({
  width,
  align = 'start',
  children,
}: {
  width: number
  align?: 'start' | 'end'
  children: ReactNode
}) {
  return (
    <th
      scope='col'
      className={cn(slots.column(), align === 'end' && 'text-end')}
      style={{ minWidth: width }}
    >
      {children}
    </th>
  )
}

/** One read-only record. Memoised on the cached record and state objects
 * (passed apart from the row wrapper, which a live patch rebuilds), so opening
 * a form, typing in it or a patch of another record does not re-render it. */
const RecordRow = memo(function RecordRow({
  record,
  source,
  state,
  lineLabel,
  cloudflare,
  onEdit,
}: ZoneRecordRow & {
  /** Omitted when the provider has no lines (Cloudflare). */
  lineLabel?: string
  cloudflare: boolean
  onEdit: (id: string) => void
}) {
  const cell = slots.cell()
  return (
    <tr className={slots.row()}>
      <td className={cell}>{record.type}</td>
      <th scope='row' className={cn(cell, 'text-start font-normal')}>
        <span title={record.name} className='block max-w-56 truncate'>
          {record.name}
        </span>
      </th>
      <td className={cell}>
        <span title={record.content} className='block max-w-80 truncate'>
          {record.type === 'MX' && record.priority !== undefined && (
            <span className='text-muted'>{record.priority} </span>
          )}
          {record.content}
        </span>
      </td>
      {lineLabel !== undefined && (
        <td className={cell}>
          <span title={lineLabel} className='block max-w-48 truncate'>
            {lineLabel}
          </span>
        </td>
      )}
      <td className={cn(cell, 'text-end tabular-nums')}>
        {cloudflare && record.ttl === 1 ? '自动' : record.ttl}
      </td>
      {cloudflare && (
        <td className={cell}>
          {record.type === 'TXT' || record.type === 'MX' ? (
            <span className='text-muted'>—</span>
          ) : record.proxied ? (
            '已代理'
          ) : (
            '仅 DNS'
          )}
        </td>
      )}
      <td className={cell}>
        {state?.last_error ? (
          <Tooltip delay={200}>
            <Tooltip.Trigger aria-label={state.last_error}>
              <StatusChip
                status={state.sync_status}
                reserve={recordSyncStatusLabels}
              />
            </Tooltip.Trigger>
            <Tooltip.Content className='max-w-xs'>
              {state.last_error}
            </Tooltip.Content>
          </Tooltip>
        ) : state ? (
          <StatusChip
            status={state.sync_status}
            reserve={recordSyncStatusLabels}
          />
        ) : (
          <span className='text-muted'>—</span>
        )}
      </td>
      <td className={cn(cell, 'text-end')}>
        {source === 'system' ? (
          <Tooltip delay={300}>
            <Tooltip.Trigger aria-label='系统记录，由集群、证书或网站自动维护'>
              <Chip size='sm' variant='soft'>
                系统
              </Chip>
            </Tooltip.Trigger>
            <Tooltip.Content>
              由集群、证书或网站自动维护，不能在此修改
            </Tooltip.Content>
          </Tooltip>
        ) : (
          <Button
            id={`dns-record-edit-${record.id}`}
            size='sm'
            variant='tertiary'
            className='px-2'
            aria-label={`编辑记录 ${record.name} ${record.type}`}
            onPress={() => onEdit(record.id)}
          >
            编辑
          </Button>
        )}
      </td>
    </tr>
  )
})
