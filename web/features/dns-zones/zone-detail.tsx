import { DataGrid, type DataGridColumn } from '@heroui-pro/react'
import { Tabs } from '@heroui/react'
import { formatDate } from '@/lib/format'
import { Drawer } from '@/components/drawer'
import { Notice } from '@/components/forms'
import { StatusChip } from '@/components/status-chip'
import { dnsLinePath } from './dns-lines'
import { displaySyncStatus } from './dns-zone-display'
import type { DnsRecord, DnsZone } from './types'

export function ZoneDetailDialog({
  zone,
  onClose,
}: {
  zone: DnsZone
  onClose: () => void
}) {
  const records = [
    ...zone.runtime.projected_records.map((record) => ({
      ...record,
      source: '系统自动',
    })),
    ...zone.config.records.map((record) => ({ ...record, source: '自定义' })),
  ]
  const columns: DataGridColumn<DnsRecord & { source: string }>[] = [
    { id: 'type', header: '类型', accessorKey: 'type', minWidth: 75 },
    {
      id: 'name',
      header: '主机记录',
      accessorKey: 'name',
      isRowHeader: true,
      minWidth: 150,
    },
    { id: 'content', header: '记录值', accessorKey: 'content', minWidth: 230 },
    { id: 'source', header: '来源', accessorKey: 'source', minWidth: 100 },
    { id: 'ttl', header: 'TTL', accessorKey: 'ttl', minWidth: 70 },
    {
      id: 'line',
      header: '线路',
      minWidth: 130,
      cell: (record) => dnsLinePath(zone.runtime.lines, record.line_code),
    },
    {
      id: 'priority',
      header: 'MX 优先级',
      minWidth: 90,
      cell: (record) => (record.type === 'MX' ? (record.priority ?? '—') : '—'),
    },
    ...(zone.dns_provider === 'cloudflare'
      ? [
          {
            id: 'proxy',
            header: '代理',
            minWidth: 90,
            cell: (record: DnsRecord) => (record.proxied ? '已代理' : '仅 DNS'),
          },
        ]
      : []),
  ]
  return (
    <Drawer title={`${zone.domain} · DNS 详情`} onClose={onClose} size='lg'>
      <Tabs defaultSelectedKey='overview'>
        <Tabs.ListContainer>
          <Tabs.List aria-label='DNS 详情'>
            <Tabs.Tab id='overview'>
              概览
              <Tabs.Indicator />
            </Tabs.Tab>
            <Tabs.Tab id='records'>
              记录
              <Tabs.Indicator />
            </Tabs.Tab>
            <Tabs.Tab id='lines'>
              解析线路
              <Tabs.Indicator />
            </Tabs.Tab>
          </Tabs.List>
        </Tabs.ListContainer>
        <Tabs.Panel id='overview' className='grid gap-4 pt-4'>
          <div className='flex flex-wrap items-center gap-3'>
            <StatusChip status={displaySyncStatus(zone)} />
            <span className='text-sm text-muted'>
              {records.length} 条记录 · {zone.website_count} 个关联网站
            </span>
          </div>
          <dl className='grid gap-3 text-sm sm:grid-cols-2'>
            {[
              ['DNS 账号', zone.dns_provider_name],
              ['最近同步', formatDate(zone.last_synced_at)],
              ['目标版本', String(zone.desired_revision)],
              ['同步版本', String(zone.synced_revision)],
            ].map(([label, value]) => (
              <div key={label}>
                <dt className='text-muted'>{label}</dt>
                <dd className='mt-1 break-all'>{value}</dd>
              </div>
            ))}
          </dl>
          {zone.last_error && <Notice>{zone.last_error}</Notice>}
          <section className='grid gap-2'>
            <h3 className='text-sm font-medium'>同步冲突</h3>
            {zone.runtime.conflicts.length ? (
              zone.runtime.conflicts.map((item) => (
                <div
                  key={item.id}
                  className='grid gap-1 rounded-xl bg-surface-secondary p-3 text-sm'
                >
                  <strong className='font-medium'>
                    {item.type} {item.name}
                  </strong>
                  <p className='break-all text-muted'>
                    本地：{item.local_content}
                  </p>
                  <p className='break-all text-muted'>
                    远端：{item.remote_content}
                  </p>
                </div>
              ))
            ) : (
              <p className='text-sm text-muted'>当前没有冲突。</p>
            )}
          </section>
        </Tabs.Panel>
        <Tabs.Panel id='records' className='pt-4'>
          <DataGrid
            aria-label='DNS 记录'
            columns={columns}
            data={records}
            getRowId={(record) => `${record.source}-${record.id}`}
            renderEmptyState={() => '暂无 DNS 记录'}
          />
        </Tabs.Panel>
        <Tabs.Panel id='lines' className='grid gap-3 pt-4'>
          <p className='text-xs text-muted'>
            最近同步：{formatDate(zone.runtime.lines_synced_at)}
          </p>
          {zone.runtime.lines.length ? (
            zone.runtime.lines.map((line) => (
              <div
                key={line.code}
                className='flex flex-wrap items-center justify-between gap-2 text-sm'
              >
                <div>
                  <p>{dnsLinePath(zone.runtime.lines, line.code)}</p>
                  <code className='text-xs text-muted'>{line.code}</code>
                </div>
                <StatusChip status={line.status} />
              </div>
            ))
          ) : (
            <p className='text-sm text-muted'>
              暂无解析线路，请在域名列表执行同步。
            </p>
          )}
        </Tabs.Panel>
      </Tabs>
    </Drawer>
  )
}
