import { dnsLinePath } from '@/lib/dns-lines'
import { formatDate } from '@/lib/format'
import type { DnsZone } from '@/lib/types'
import { Badge } from '@/components/ui/badge'
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from '@/components/ui/table'
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetHeader,
  SheetTitle,
} from '@/components/ui/sheet'
import {
  Tabs,
  TabsContent,
  TabsList,
  TabsTrigger,
} from '@/components/ui/tabs'
import { DnsLineTree } from '@/components/dns-line-tree'
import { StatusBadge } from '@/components/status-badge'
import { displaySyncStatus } from './dns-zone-display'

export function ZoneDetailSheet({
  zone,
  onOpenChange,
}: {
  zone: DnsZone | null
  onOpenChange: (open: boolean) => void
}) {
  const supportsProxy = zone?.dns_provider === 'cloudflare'
  const hasMxRecords =
    zone?.config.records.some((record) => record.type === 'MX') ?? false
  const conflicts = zone
    ? zone.runtime.conflicts.filter(
        (conflict) => conflict.local_content !== conflict.remote_content
      )
    : []

  return (
    <Sheet open={!!zone} onOpenChange={onOpenChange}>
      <SheetContent className='flex h-full w-full flex-col overflow-hidden p-0 sm:max-w-4xl'>
        <SheetHeader className='shrink-0 px-6 pt-6 text-start'>
          <SheetTitle>{zone?.domain} · DNS 详情</SheetTitle>
          <SheetDescription>
            {zone?.dns_provider_name} · 查看基础信息、DNS 记录和线路。
          </SheetDescription>
        </SheetHeader>
        {zone && (
          <Tabs
            defaultValue='overview'
            className='min-h-0 flex-1 gap-4 px-6 pb-4'
          >
            <TabsList className='grid w-full shrink-0 grid-cols-3'>
              <TabsTrigger value='overview'>基础信息</TabsTrigger>
              <TabsTrigger value='records'>记录</TabsTrigger>
              <TabsTrigger value='lines'>线路</TabsTrigger>
            </TabsList>
            <TabsContent
              value='overview'
              className='min-h-0 flex-1 space-y-4 overflow-y-auto'
            >
              <div className='grid gap-3 sm:grid-cols-3'>
                <div className='rounded-md border p-3'>
                  <div className='text-xs text-muted-foreground'>本地/远端同步</div>
                  <div className='mt-2'>
                    <StatusBadge status={displaySyncStatus(zone)} />
                  </div>
                </div>
                <div className='rounded-md border p-3'>
                  <div className='text-xs text-muted-foreground'>DNS 记录</div>
                  <div className='mt-1 text-2xl font-semibold'>
                    {zone.config.records.length}
                  </div>
                </div>
                <div className='rounded-md border p-3'>
                  <div className='text-xs text-muted-foreground'>DNS 线路</div>
                  <div className='mt-1 text-2xl font-semibold'>
                    {zone.runtime.lines.length}
                  </div>
                </div>
              </div>
              <dl className='grid gap-3 rounded-md border p-4 text-sm sm:grid-cols-2'>
                <div>
                  <dt className='text-muted-foreground'>托管域名</dt>
                  <dd className='mt-1 font-medium'>{zone.domain}</dd>
                </div>
                <div>
                  <dt className='text-muted-foreground'>DNS 服务商</dt>
                  <dd className='mt-1 font-medium'>{zone.dns_provider_name}</dd>
                </div>
                <div>
                  <dt className='text-muted-foreground'>最近同步</dt>
                  <dd className='mt-1 font-medium'>
                    {formatDate(zone.last_synced_at)}
                  </dd>
                </div>
              </dl>
              <div>
                <h3 className='mb-2 flex items-center text-sm font-semibold'>
                  冲突
                  <Badge variant='secondary' className='ms-2'>
                    {conflicts.length}
                  </Badge>
                </h3>
                <div className='space-y-2'>
                  {conflicts.map((conflict) => (
                    <div
                      key={conflict.id}
                      className='rounded-md border p-3 text-sm'
                    >
                      <div className='font-medium'>
                        {conflict.type} {conflict.name}
                      </div>
                      <div className='mt-2 grid gap-1 text-xs'>
                        <div>本地：{conflict.local_content}</div>
                        <div>远端：{conflict.remote_content}</div>
                      </div>
                    </div>
                  ))}
                  {!conflicts.length && (
                    <p className='text-sm text-muted-foreground'>
                      当前没有冲突。
                    </p>
                  )}
                </div>
              </div>
              {zone.last_error && (
                <div className='rounded-md border border-destructive/30 bg-destructive/5 p-3 text-sm text-destructive'>
                  {zone.last_error}
                </div>
              )}
            </TabsContent>
            <TabsContent
              value='records'
              className='min-h-0 flex-1 overflow-hidden'
            >
              <Table
                containerClassName='h-full overflow-auto overscroll-contain'
                containerLabel='DNS记录详情表格'
              >
                <TableHeader className='sticky top-0 z-10 bg-background'>
                  <TableRow>
                    <TableHead>类型</TableHead>
                    <TableHead>主机记录</TableHead>
                    <TableHead>记录值</TableHead>
                    {hasMxRecords && <TableHead>优先级</TableHead>}
                    <TableHead>TTL</TableHead>
                    <TableHead>线路</TableHead>
                    {supportsProxy && <TableHead>代理</TableHead>}
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {zone.config.records.map((record) => (
                    <TableRow key={record.id}>
                      <TableCell>{record.type}</TableCell>
                      <TableCell>{record.name}</TableCell>
                      <TableCell>{record.content}</TableCell>
                      {hasMxRecords && (
                        <TableCell>
                          {record.type === 'MX' ? record.priority ?? '—' : '—'}
                        </TableCell>
                      )}
                      <TableCell>{record.ttl}</TableCell>
                      <TableCell>
                        {dnsLinePath(zone.runtime.lines, record.line_code)}
                      </TableCell>
                      {supportsProxy && (
                        <TableCell>
                          <Badge variant='secondary'>
                            {record.proxied ? '已代理' : '仅 DNS'}
                          </Badge>
                        </TableCell>
                      )}
                    </TableRow>
                  ))}
                  {!zone.config.records.length && (
                    <TableRow>
                      <TableCell
                        colSpan={(supportsProxy ? 6 : 5) + (hasMxRecords ? 1 : 0)}
                        className='h-24 text-center text-muted-foreground'
                      >
                        暂无 DNS 记录
                      </TableCell>
                    </TableRow>
                  )}
                </TableBody>
              </Table>
            </TabsContent>
            <TabsContent
              value='lines'
              className='min-h-0 flex-1 overflow-hidden'
            >
              <DnsLineTree lines={zone.runtime.lines} />
            </TabsContent>
          </Tabs>
        )}
      </SheetContent>
    </Sheet>
  )
}

