import { useMemo, useState } from 'react'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Chip, Tooltip } from '@heroui/react'
import { pageLocally, useListState } from '@/lib/list-search'
import { describeStatus } from '@/lib/status'
import { DataTable } from '@/components/data-table'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { dnsLineIndex } from './dns-lines'
import {
  recordSyncStatuses,
  recordSyncStatusReserve,
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

type OpenForm = { kind: 'add' } | { kind: 'edit'; record: DnsRecord }

/** The records tab keeps filters, pagination and its record sheet in
 * component state. System records are read-only; custom records are edited
 * in a sheet so the DataGrid owns table navigation and the sheet owns input. */
export function ZoneRecords() {
  const zone = useZoneDetail().data
  const list = useListState<RecordsSearch>()
  const [open, setOpen] = useState<OpenForm | null>(null)
  const custom = zone?.config.records
  const system = zone?.runtime.projected_records
  const states = zone?.runtime.record_states
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
  if (!zone) return null
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
  const columns: DataGridColumn<ZoneRecordRow>[] = [
    {
      id: 'type',
      header: '类型',
      minWidth: 88,
      cell: ({ record }) => record.type,
    },
    {
      id: 'name',
      header: '名称',
      isRowHeader: true,
      minWidth: 170,
      cell: ({ record }) => (
        <span title={record.name} className='block max-w-56 truncate'>
          {record.name}
        </span>
      ),
    },
    {
      id: 'content',
      header: '内容',
      minWidth: 240,
      cell: ({ record }) => (
        <span title={record.content} className='block max-w-80 truncate'>
          {record.type === 'MX' && record.priority !== undefined && (
            <span className='text-muted'>{record.priority} </span>
          )}
          {record.content}
        </span>
      ),
    },
    ...(showLines
      ? [
          {
            id: 'line',
            header: '线路',
            minWidth: 150,
            cell: ({ record }: ZoneRecordRow) => (
              <span className='block max-w-48 truncate'>
                {lineLabels.get(record.line_code) ?? record.line_code}
              </span>
            ),
          },
        ]
      : []),
    {
      id: 'ttl',
      header: 'TTL',
      minWidth: 80,
      align: 'end',
      cell: ({ record }) => (
        <span className='tabular-nums'>
          {cloudflare && record.ttl === 1 ? '自动' : record.ttl}
        </span>
      ),
    },
    ...(cloudflare
      ? [
          {
            id: 'proxy',
            header: '代理',
            minWidth: 90,
            cell: ({ record }: ZoneRecordRow) =>
              record.type === 'TXT' || record.type === 'MX' ? (
                <span className='text-muted'>—</span>
              ) : record.proxied ? (
                '已代理'
              ) : (
                '仅 DNS'
              ),
          },
        ]
      : []),
    {
      id: 'sync',
      header: '同步',
      minWidth: 110,
      cell: ({ state }) =>
        state?.last_error ? (
          <Tooltip delay={200}>
            <Tooltip.Trigger aria-label={state.last_error}>
              <StatusChip
                {...describeStatus(recordSyncStatuses, state.sync_status)}
                reserve={recordSyncStatusReserve}
              />
            </Tooltip.Trigger>
            <Tooltip.Content className='max-w-xs'>
              {state.last_error}
            </Tooltip.Content>
          </Tooltip>
        ) : state ? (
          <StatusChip
            {...describeStatus(recordSyncStatuses, state.sync_status)}
            reserve={recordSyncStatusReserve}
          />
        ) : (
          <span className='text-muted'>—</span>
        ),
    },
    {
      id: 'actions',
      header: <span className='sr-only'>操作</span>,
      minWidth: 80,
      align: 'end',
      cell: ({ record, source }) =>
        source === 'system' ? (
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
            size='sm'
            variant='tertiary'
            className='px-2'
            aria-label={`编辑记录 ${record.name} ${record.type}`}
            onPress={() => setOpen({ kind: 'edit', record })}
          >
            编辑
          </Button>
        ),
    },
  ]
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
            size='sm'
            isDisabled={zone.config.records.length >= maxRecords}
            onPress={() => setOpen({ kind: 'add' })}
          >
            <Plus className='size-4' />
            添加记录
          </Button>
        }
      />
      <DataTable
        label='DNS 记录'
        variant='secondary'
        columns={columns}
        data={page.list}
        isLoading={false}
        getRowId={({ record, source }) => `${source}:${record.id}`}
        minWidth={860}
        empty={{
          title: filtered ? '没有匹配的记录' : '还没有 DNS 记录',
          description: filtered ? '调整搜索词或筛选条件后重试。' : undefined,
        }}
        footer={
          <TablePagination
            page={page.page}
            pageSize={list.pageSize}
            total={page.total}
            onPageChange={list.setPage}
            onPageSizeChange={list.setPageSize}
          />
        }
      />
      {open && (
        <RecordForm
          key={open.kind === 'edit' ? open.record.id : 'new'}
          zone={zone}
          record={open.kind === 'edit' ? open.record : undefined}
          showLines={showLines}
          onClose={() => setOpen(null)}
        />
      )}
    </Section>
  )
}
