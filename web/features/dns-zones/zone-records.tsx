import { getRouteApi } from '@tanstack/react-router'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Tooltip } from '@heroui/react'
import { useListSearch } from '@/lib/list-search'
import { DataTable } from '@/components/data-table'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { dnsLinePath } from './dns-lines'
import { zoneRecordRows, type ZoneRecordRow } from './dns-zone-display'
import { RecordsEditor } from './records-editor'
import { useZoneDetail } from './use-zone-detail'

const route = getRouteApi('/_authenticated/dns-zones/$zoneId/records')
const filterKeys = ['type', 'source'] as const

export function ZoneRecords() {
  const zone = useZoneDetail().data
  const list = useListSearch(route)
  const navigate = route.useNavigate()
  if (!zone) return null
  const setEditing = (edit: boolean) =>
    void navigate({
      search: (previous) => ({ ...previous, edit: edit ? 1 : undefined }),
      replace: true,
    })
  if (list.search.edit === 1)
    return (
      <RecordsEditor
        key={zone.id}
        zone={zone}
        onExit={() => setEditing(false)}
      />
    )
  const keyword = list.keyword.toLowerCase()
  const rows = zoneRecordRows(zone).filter(
    (record) =>
      (!list.search.type || record.type === list.search.type) &&
      (!list.search.source || record.source === list.search.source) &&
      (!keyword ||
        record.name.toLowerCase().includes(keyword) ||
        record.content.toLowerCase().includes(keyword))
  )
  const filtered = !!keyword || !!list.search.type || !!list.search.source
  const cloudflare = zone.dns_provider === 'cloudflare'
  const columns: DataGridColumn<ZoneRecordRow>[] = [
    { id: 'type', header: '类型', width: 80, accessorKey: 'type' },
    {
      id: 'name',
      header: '主机记录',
      isRowHeader: true,
      minWidth: 150,
      cell: (record) => <Mono>{record.name}</Mono>,
    },
    {
      id: 'content',
      header: '记录值',
      minWidth: 220,
      cell: (record) => (
        <Mono className='break-all'>
          {record.type === 'MX' && record.priority !== undefined && (
            <span className='text-muted'>{record.priority} </span>
          )}
          {record.content}
        </Mono>
      ),
    },
    {
      id: 'line',
      header: '线路',
      minWidth: 120,
      cell: (record) => dnsLinePath(zone.runtime.lines, record.line_code),
    },
    {
      id: 'ttl',
      header: 'TTL',
      align: 'end',
      width: 80,
      cell: (record) => (
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
            cell: (record: ZoneRecordRow) =>
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
      id: 'source',
      header: '来源',
      minWidth: 90,
      cell: (record) =>
        record.source === 'system' ? (
          <span className='text-muted'>系统自动</span>
        ) : (
          '自定义'
        ),
    },
    {
      id: 'state',
      header: '同步',
      minWidth: 110,
      cell: (record) =>
        record.state?.last_error ? (
          <Tooltip delay={200}>
            <Tooltip.Trigger aria-label={record.state.last_error}>
              <StatusChip status={record.state.sync_status} />
            </Tooltip.Trigger>
            <Tooltip.Content className='max-w-xs'>
              {record.state.last_error}
            </Tooltip.Content>
          </Tooltip>
        ) : record.state ? (
          <StatusChip status={record.state.sync_status} />
        ) : (
          <span className='text-muted'>—</span>
        ),
    },
  ]
  return (
    <Section
      title='DNS 记录'
      description='系统自动记录由集群、证书等资源生成；自定义记录可在编辑模式中修改，保存后同步到服务商。'
      contentClassName='flex flex-col gap-3'
    >
      <ListToolbar
        search={{
          value: list.keyword,
          onChange: (q) => void list.setSearch({ q }),
          placeholder: '搜索主机记录或记录值',
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
              options={['A', 'AAAA', 'CNAME', 'TXT', 'MX'].map((id) => ({
                id,
                label: id,
              }))}
            />
            <FilterSelect
              label='来源'
              value={list.search.source ?? ''}
              onChange={(value) =>
                void list.setSearch({
                  source: (value || undefined) as typeof list.search.source,
                })
              }
              options={[
                { id: 'custom', label: '自定义' },
                { id: 'system', label: '系统自动' },
              ]}
            />
          </>
        }
        hasFilters={filtered}
        onClearFilters={() => void list.clearFilters(filterKeys)}
      />
      <DataTable
        label='DNS 记录'
        columns={columns}
        data={rows}
        isLoading={false}
        getRowId={(record) => `${record.source}-${record.id}`}
        minWidth={860}
        empty={
          filtered
            ? {
                title: '没有匹配的记录',
                description: '调整搜索词或筛选条件后重试。',
              }
            : {
                title: '还没有 DNS 记录',
                description: '添加自定义记录后保存，即可同步到服务商。',
                action: (
                  <Button size='sm' onPress={() => setEditing(true)}>
                    编辑记录
                  </Button>
                ),
              }
        }
      />
    </Section>
  )
}
