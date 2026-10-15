import type { DataGridColumn } from '@heroui-pro/react'
import { formatDate } from '@/lib/format'
import { pageLocally, useListState } from '@/lib/list-search'
import { DataTable } from '@/components/data-table'
import { Mono } from '@/components/mono'
import { Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { type DnsLineTreeOption, dnsLineIndex } from './dns-lines'
import { lineStatusLabels } from './dns-zone-display'
import { useZoneDetail } from './use-zone-detail'

const columns: DataGridColumn<DnsLineTreeOption>[] = [
  {
    id: 'path',
    header: '线路',
    isRowHeader: true,
    minWidth: 240,
    cell: ({ path }) => (
      <span>
        {path.slice(0, -1).map((segment) => (
          <span key={segment} className='text-muted'>
            {segment} /{' '}
          </span>
        ))}
        {path.at(-1)}
      </span>
    ),
  },
  {
    id: 'code',
    header: '线路代码',
    minWidth: 160,
    cell: ({ line }) => <Mono>{line.code}</Mono>,
  },
  {
    id: 'status',
    header: '状态',
    minWidth: 110,
    cell: ({ line }) => (
      <StatusChip status={line.status} reserve={lineStatusLabels} />
    ),
  },
]

/** Provider lines from the zone snapshot's shared line index, paged on the
 * client (Aliyun lists hundreds); page and page size are component state. */
export function ZoneLines() {
  const zone = useZoneDetail().data
  const list = useListState()
  if (!zone) return null
  const { options } = dnsLineIndex(zone.runtime.lines)
  const page = pageLocally(options, list.page, list.pageSize)
  return (
    <Section
      title={`解析线路 · ${options.length}`}
      description={`服务商支持的解析线路，记录和节点服务端点按线路分别解析。最近更新：${formatDate(zone.runtime.lines_synced_at)}`}
    >
      <DataTable
        label='解析线路'
        columns={columns}
        data={page.list}
        isLoading={false}
        getRowId={({ line }) => line.code}
        minWidth={560}
        empty={{
          title: '暂无解析线路',
          description: '执行一次同步后会从服务商读取可用线路。',
        }}
        footer={
          <TablePagination
            page={list.page}
            pageSize={list.pageSize}
            total={page.total}
            onPageChange={list.setPage}
            onPageSizeChange={list.setPageSize}
          />
        }
      />
    </Section>
  )
}
