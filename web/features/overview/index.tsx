import { useQuery } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import {
  ArrowRotateLeft,
  ArrowRight,
  Globe,
  Layers,
  ShieldCheck,
  Link,
} from '@gravity-ui/icons'
import { DataGrid, type DataGridColumn } from '@heroui-pro/react'
import { Button, Card, Skeleton } from '@heroui/react'
import { formatDate } from '@/lib/format'
import { Page, QueryNotice, EmptyState } from '@/components/page'
import { StatusChip } from '@/components/status-chip'
import { overviewQuery } from './data'

export function OverviewPage() {
  const query = useQuery(overviewQuery),
    navigate = useNavigate(),
    data = query.data
  const cards = [
    {
      label: '网站',
      value: data?.resources.website_count,
      icon: Globe,
      href: '/websites',
    },
    {
      label: '域名',
      value: data?.resources.domain_count,
      icon: Link,
      href: '/websites',
    },
    {
      label: '证书',
      value: data?.resources.certificate_count,
      icon: ShieldCheck,
      href: '/certificates',
    },
    {
      label: '集群',
      value: data?.resources.cluster_count,
      icon: Layers,
      href: '/clusters',
    },
  ]
  type Marker = NonNullable<typeof data>['recent_markers'][number]
  const columns: DataGridColumn<Marker>[] = [
    {
      id: 'resource_name',
      header: '资源',
      isRowHeader: true,
      minWidth: 180,
      cell: (item) => <span className='font-medium'>{item.resource_name}</span>,
    },
    {
      id: 'operation',
      header: '操作',
      cell: (item) => (
        <span>
          {{
            sync: '同步',
            issue: '签发',
            renew: '续期',
            verify: '验证',
            delete: '删除',
            deploy: '发布',
          }[item.operation] ?? item.operation}
        </span>
      ),
    },
    {
      id: 'status',
      header: '状态',
      cell: (item) => <StatusChip status={item.status} />,
    },
    {
      id: 'updated_at',
      header: '更新时间',
      minWidth: 180,
      cell: (item) => (
        <span className='text-muted tabular-nums'>
          {formatDate(item.updated_at)}
        </span>
      ),
    },
  ]
  return (
    <Page
      title='概览'
      description='查看边缘网络资源与正在执行的工作。'
      actions={
        <Button
          size='sm'
          variant='tertiary'
          isPending={query.isFetching}
          onPress={() => void query.refetch()}
        >
          <ArrowRotateLeft className='size-4' />
          刷新
        </Button>
      }
    >
      <QueryNotice query={query} onRetry={() => query.refetch()} />
      <div className='grid gap-4 sm:grid-cols-2 xl:grid-cols-4'>
        {cards.map((item) => (
          <Card key={item.label}>
            <Card.Content className='flex flex-col gap-5'>
              <div className='flex items-center justify-between'>
                <span className='text-sm text-muted'>{item.label}</span>
                <item.icon className='size-5 text-accent' />
              </div>
              {query.isPending ? (
                <Skeleton className='h-9 w-16 rounded-lg' />
              ) : (
                <p className='text-3xl font-semibold tracking-tight tabular-nums'>
                  {item.value ?? '—'}
                </p>
              )}
              <Button
                size='sm'
                variant='ghost'
                className='self-start px-0'
                onPress={() => void navigate({ to: item.href })}
              >
                管理{item.label}
                <ArrowRight className='size-4' />
              </Button>
            </Card.Content>
          </Card>
        ))}
      </div>
      {data && (
        <div className='grid gap-6 lg:grid-cols-[1fr_2fr]'>
          <section className='flex flex-col gap-4'>
            <h2 className='text-base font-semibold'>需要关注</h2>
            <div className='flex flex-col gap-3 rounded-2xl bg-surface p-5'>
              {[
                {
                  label: 'DNS 同步异常',
                  count: data.issues.dns_zone_issue_count,
                  href: '/dns-zones',
                },
                {
                  label: '证书即将到期',
                  count: data.issues.certificate_expiring_count,
                  href: '/certificates',
                },
                {
                  label: '证书异常',
                  count: data.issues.certificate_failed_count,
                  href: '/certificates',
                },
                {
                  label: '执行中的任务',
                  count: data.issues.active_marker_count,
                  href: '/tasks',
                },
                {
                  label: '等待重试',
                  count: data.issues.retry_marker_count,
                  href: '/tasks',
                },
              ].map((item) => (
                <Button
                  key={item.label}
                  variant='ghost'
                  size='sm'
                  className='justify-between'
                  onPress={() => void navigate({ to: item.href })}
                >
                  <span>{item.label}</span>
                  <span className='tabular-nums'>{item.count}</span>
                </Button>
              ))}
            </div>
          </section>
          <section className='flex min-w-0 flex-col gap-4'>
            <div className='flex items-center justify-between'>
              <h2 className='text-base font-semibold'>最近任务</h2>
              <Button
                size='sm'
                variant='ghost'
                onPress={() => void navigate({ to: '/tasks' })}
              >
                查看全部
                <ArrowRight className='size-4' />
              </Button>
            </div>
            {data.recent_markers.length ? (
              <DataGrid
                aria-label='最近任务'
                data={data.recent_markers}
                columns={columns}
                getRowId={(item) => item.id}
                contentClassName='min-w-[560px]'
              />
            ) : (
              <EmptyState
                title='暂无任务'
                description='创建资源或提交配置后，可在这里查看执行情况。'
              />
            )}
          </section>
        </div>
      )}
    </Page>
  )
}
