import { useQuery } from '@tanstack/react-query'
import { Link } from '@tanstack/react-router'
import {
  AlertTriangle,
  Boxes,
  Globe2,
  Network,
  RefreshCw,
  ShieldCheck,
} from 'lucide-react'
import { getData } from '@/lib/api'
import { fromNow } from '@/lib/format'
import type { OverviewData } from '@/lib/types'
import { Button } from '@/components/ui/button'
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card'
import { Skeleton } from '@/components/ui/skeleton'
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs'
import { FeatureShell } from '@/components/feature-shell'
import { MetricCard } from '@/components/metric-card'
import { StatusBadge } from '@/components/status-badge'

export function Overview() {
  const query = useQuery({
    queryKey: ['overview'],
    queryFn: () => getData<OverviewData>('/overview/'),
  })
  const data = query.data
  const resources = data?.resources
  const issues = data?.issues
  const resourceValues = [
    { label: '网站', value: resources?.website_count ?? 0 },
    { label: '域名', value: resources?.domain_count ?? 0 },
    { label: '证书', value: resources?.certificate_count ?? 0 },
    { label: '集群', value: resources?.cluster_count ?? 0 },
  ]
  const maximum = Math.max(...resourceValues.map((item) => item.value), 1)

  return (
    <FeatureShell
      title='运行概览'
      description='查看边缘资源、风险事项和最近任务。'
      actions={
        <Button
          variant='outline'
          onClick={() => query.refetch()}
          disabled={query.isFetching}
        >
          <RefreshCw
            className={query.isFetching ? 'animate-spin' : undefined}
          />
          刷新
        </Button>
      }
    >
      {query.isError ? (
        <Card role='alert'>
          <CardContent className='flex min-h-64 flex-col items-center justify-center text-center'>
            <div className='mb-3 flex size-10 items-center justify-center rounded-full bg-destructive/10'>
              <AlertTriangle className='size-5 text-destructive' />
            </div>
            <p className='font-medium'>概览数据加载失败</p>
            <p className='mt-1 text-sm text-muted-foreground'>
              请检查网络连接后重新加载。
            </p>
            <Button
              variant='outline'
              size='sm'
              className='mt-4'
              onClick={() => void query.refetch()}
            >
              <RefreshCw /> 重新加载
            </Button>
          </CardContent>
        </Card>
      ) : (
        <Tabs defaultValue='overview' className='space-y-4'>
          <TabsList>
            <TabsTrigger value='overview'>资源概览</TabsTrigger>
            <TabsTrigger value='issues'>风险与任务</TabsTrigger>
          </TabsList>
          <TabsContent value='overview' className='space-y-4'>
            <div className='grid gap-4 sm:grid-cols-2 lg:grid-cols-4'>
              {query.isLoading ? (
                Array.from({ length: 4 }).map((_, index) => (
                  <Skeleton key={index} className='h-32 rounded-xl' />
                ))
              ) : (
                <>
                  <MetricCard
                    label='网站'
                    value={resources?.website_count ?? 0}
                    hint='已纳入边缘调度'
                    icon={Globe2}
                  />
                  <MetricCard
                    label='托管域名'
                    value={resources?.domain_count ?? 0}
                    hint='DNS 配置资源'
                    icon={Network}
                  />
                  <MetricCard
                    label='证书'
                    value={resources?.certificate_count ?? 0}
                    hint='自动签发与续期'
                    icon={ShieldCheck}
                  />
                  <MetricCard
                    label='集群'
                    value={resources?.cluster_count ?? 0}
                    hint='边缘资源分组'
                    icon={Boxes}
                  />
                </>
              )}
            </div>
            <div className='grid grid-cols-1 gap-4 lg:grid-cols-7'>
              <Card className='lg:col-span-4'>
                <CardHeader>
                  <CardTitle>资源分布</CardTitle>
                  <CardDescription>当前控制台已管理资源数量</CardDescription>
                </CardHeader>
                <CardContent>
                  <div className='space-y-5'>
                    {resourceValues.map((item) => (
                      <div key={item.label}>
                        <div className='mb-2 flex items-center justify-between text-sm'>
                          <span>{item.label}</span>
                          <span className='font-medium tabular-nums'>
                            {item.value}
                          </span>
                        </div>
                        <div className='h-2.5 overflow-hidden rounded-full bg-muted'>
                          <div
                            className='h-full rounded-full bg-primary'
                            style={{
                              width: `${Math.max(4, (item.value / maximum) * 100)}%`,
                            }}
                          />
                        </div>
                      </div>
                    ))}
                  </div>
                </CardContent>
              </Card>
              <Card className='lg:col-span-3'>
                <CardHeader>
                  <CardTitle>最近任务</CardTitle>
                  <CardDescription>
                    {data?.recent_tasks.length
                      ? `最近 ${data.recent_tasks.length} 条变更`
                      : '暂无任务记录'}
                  </CardDescription>
                </CardHeader>
                <CardContent className='space-y-5'>
                  {data?.recent_tasks.slice(0, 5).map((task) => (
                    <div key={task.id} className='flex items-start gap-3'>
                      <div className='mt-1 size-2 rounded-full bg-primary' />
                      <div className='min-w-0 flex-1'>
                        <div className='flex items-center justify-between gap-2'>
                          <p className='truncate text-sm font-medium'>
                            {task.resource_name}
                          </p>
                          <StatusBadge status={task.status} />
                        </div>
                        <p className='mt-1 truncate text-xs text-muted-foreground'>
                          {task.operation} · {fromNow(task.updated_at)}
                        </p>
                      </div>
                    </div>
                  ))}
                  {data?.recent_tasks.length ? (
                    <Button variant='outline' className='w-full' asChild>
                      <Link to='/tasks'>查看全部任务</Link>
                    </Button>
                  ) : null}
                </CardContent>
              </Card>
            </div>
          </TabsContent>
          <TabsContent value='issues' className='space-y-4'>
            <div className='grid gap-4 sm:grid-cols-2 lg:grid-cols-4'>
              <MetricCard
                label='DNS 异常'
                value={issues?.dns_zone_issue_count ?? 0}
                hint='需要检查同步状态'
                icon={Network}
              />
              <MetricCard
                label='即将过期'
                value={issues?.certificate_expiring_count ?? 0}
                hint='证书续期窗口'
                icon={ShieldCheck}
              />
              <MetricCard
                label='签发失败'
                value={issues?.certificate_failed_count ?? 0}
                hint='需要人工处理'
                icon={AlertTriangle}
              />
              <MetricCard
                label='活动任务'
                value={issues?.active_task_count ?? 0}
                hint={`${issues?.failed_task_count ?? 0} 条失败任务`}
                icon={RefreshCw}
              />
            </div>
          </TabsContent>
        </Tabs>
      )}
    </FeatureShell>
  )
}
