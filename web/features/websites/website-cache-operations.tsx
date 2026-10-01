import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { DataGrid, type DataGridColumn } from '@heroui-pro/react'
import { Button, Tabs, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { formatBytes, formatDate } from '@/lib/format'
import { queryKeys } from '@/lib/query-keys'
import { Confirm } from '@/components/confirm'
import {
  Choice,
  Dialog,
  Field,
  Notice,
  TextAreaField,
} from '@/components/forms'
import { ResourceList } from '@/components/resource-list'
import { RowMenu } from '@/components/row-menu'
import { StatusChip } from '@/components/status-chip'
import { canCancelCacheJob, canRetryCacheJob } from './cache-job-actions'
import {
  cacheJobsQuery,
  cancelCacheJob,
  createCacheJob,
  retryCacheJob,
} from './cache-job-data'
import { parseCacheJobInput, type CacheJobInput } from './cache-job-form'
import type { CacheJob } from './cache-job-types'
import type { Website } from './types'

export function WebsiteCacheOperations({
  website,
  onClose,
}: {
  website: Website
  onClose: () => void
}) {
  const client = useQueryClient()
  const [tab, setTab] = useState('purge')
  const [mode, setMode] = useState<CacheJobInput['mode']>('url')
  const [targets, setTargets] = useState('')
  const [concurrency, setConcurrency] = useState('2')
  const [rate, setRate] = useState('5')
  const [error, setError] = useState('')
  const [page, setPage] = useState(1)
  const [confirmation, setConfirmation] = useState<CacheJobInput | null>(null)
  const [cancelTarget, setCancelTarget] = useState<CacheJob | null>(null)
  const [detailId, setDetailId] = useState<string | null>(null)
  const jobs = useQuery({
    ...cacheJobsQuery(website.id, { page, page_size: 20 }),
    enabled: tab === 'jobs',
  })
  const invalidate = () =>
    void client.invalidateQueries({
      queryKey: [...queryKeys.websites, website.id, 'cache-jobs'],
    })
  const create = useMutation({
    mutationFn: (input: CacheJobInput) =>
      createCacheJob(website.id, website.revision, input),
    onSuccess: () => {
      toast.success('缓存任务已创建')
      setConfirmation(null)
      setTab('jobs')
      setPage(1)
      invalidate()
    },
    onError: (failure) => setError(apiErrorMessage(failure)),
  })
  const cancel = useMutation({
    mutationFn: cancelCacheJob,
    onSuccess: () => {
      toast.success('取消请求已提交')
      setCancelTarget(null)
      invalidate()
    },
    onError: (failure) => toast.danger(apiErrorMessage(failure)),
  })
  const retry = useMutation({
    mutationFn: retryCacheJob,
    onSuccess: () => {
      toast.success('重试任务已创建')
      invalidate()
    },
    onError: (failure) => toast.danger(apiErrorMessage(failure)),
  })
  function submit() {
    setError('')
    const checked = parseCacheJobInput(website, {
      operation: tab === 'preheat' ? 'preheat' : 'purge',
      mode,
      targets,
      concurrency,
      rate_mib: rate,
    })
    if (!checked.ok) {
      setError(checked.error)
      return
    }
    const { input } = checked
    if (input.operation === 'purge') setConfirmation(input)
    else create.mutate(input)
  }
  const columns: DataGridColumn<CacheJob>[] = [
    {
      id: 'operation',
      header: '任务',
      isRowHeader: true,
      minWidth: 130,
      cell: (job) => (
        <Button size='sm' variant='ghost' onPress={() => setDetailId(job.id)}>
          {job.operation === 'purge' ? '缓存刷新' : '缓存预热'}
        </Button>
      ),
    },
    {
      id: 'scope',
      header: '范围',
      minWidth: 150,
      cell: (job) =>
        job.mode === 'all'
          ? '全部缓存'
          : `${job.mode === 'prefix' ? '目录' : 'URL'} · ${job.targets.length} 项`,
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 110,
      cell: (job) => <StatusChip status={job.status} />,
    },
    {
      id: 'nodes',
      header: '节点进度',
      minWidth: 110,
      cell: (job) => (
        <span className='tabular-nums'>
          {job.nodes.filter((node) => node.status === 'completed').length} /{' '}
          {job.nodes.length}
        </span>
      ),
    },
    {
      id: 'created',
      header: '创建时间',
      minWidth: 160,
      cell: (job) => formatDate(job.created_at),
    },
    {
      id: 'actions',
      header: '操作',
      width: 70,
      cell: (job) => (
        <RowMenu
          label='缓存任务操作'
          actions={[
            {
              id: 'detail',
              label: '查看结果',
              onAction: () => setDetailId(job.id),
            },
            {
              id: 'cancel',
              label: '取消任务',
              danger: true,
              disabled: !canCancelCacheJob(job) || cancel.isPending,
              onAction: () => setCancelTarget(job),
            },
            {
              id: 'retry',
              label: '重试失败节点',
              disabled: !canRetryCacheJob(job) || retry.isPending,
              onAction: () => retry.mutate(job),
            },
          ]}
        />
      ),
    },
  ]
  const detail = jobs.data?.list.find((job) => job.id === detailId)
  return (
    <>
      <Dialog
        title={`${website.config.name || website.access_domain} · 缓存`}
        onClose={onClose}
        size='lg'
        busy={create.isPending || cancel.isPending}
      >
        <div className='space-y-5'>
          <p className='text-sm text-muted'>
            刷新缓存会让下一次请求回源，预热把目标资源提前写入缓存。
          </p>
          <Tabs
            selectedKey={tab}
            onSelectionChange={(key) => {
              setTab(String(key))
              setError('')
            }}
            variant='secondary'
          >
            <Tabs.ListContainer>
              <Tabs.List aria-label='缓存操作'>
                <Tabs.Tab id='purge' isDisabled={create.isPending}>
                  刷新
                  <Tabs.Indicator />
                </Tabs.Tab>
                <Tabs.Tab id='preheat' isDisabled={create.isPending}>
                  预热
                  <Tabs.Indicator />
                </Tabs.Tab>
                <Tabs.Tab id='jobs' isDisabled={create.isPending}>
                  任务结果
                  <Tabs.Indicator />
                </Tabs.Tab>
              </Tabs.List>
            </Tabs.ListContainer>
            <Tabs.Panel id={tab} className='space-y-5 pt-4'>
              <Notice>{error}</Notice>
              {tab === 'jobs' ? (
                <>
                  <ResourceList
                    label='缓存任务'
                    query={jobs}
                    columns={columns}
                    page={page}
                    onPageChange={setPage}
                    onRefresh={() => void jobs.refetch()}
                    emptyTitle='暂无缓存任务'
                    emptyDescription='提交刷新或预热后在此查看节点执行结果。'
                  />
                  {detail && (
                    <section className='space-y-3 rounded-2xl bg-surface-secondary p-4'>
                      <div className='flex flex-wrap items-center justify-between gap-2'>
                        <h3 className='text-sm font-medium'>
                          任务结果 · {detail.id.slice(0, 8)}
                        </h3>
                        <Button
                          size='sm'
                          variant='tertiary'
                          onPress={() => setDetailId(null)}
                        >
                          收起
                        </Button>
                      </div>
                      <p className='text-xs text-muted'>
                        网站版本 {detail.website_revision} · 策略版本{' '}
                        {detail.policy_revision} · 更新于{' '}
                        {formatDate(detail.updated_at)}
                      </p>
                      <Notice>{detail.error}</Notice>
                      {detail.targets.length > 0 && (
                        <div className='space-y-1 text-xs'>
                          {detail.targets.map((target) => (
                            <code key={target} className='block break-all'>
                              {target}
                            </code>
                          ))}
                        </div>
                      )}
                      {detail.nodes.map((node) => (
                        <div key={node.node_id} className='space-y-2'>
                          <div className='flex flex-wrap items-center gap-2'>
                            <span className='text-sm font-medium'>
                              {node.node_name}
                            </span>
                            <StatusChip status={node.status} />
                          </div>
                          <Notice>{node.error}</Notice>
                          {node.results.length > 0 && (
                            <DataGrid
                              aria-label={`${node.node_name} 缓存执行结果`}
                              data={node.results}
                              getRowId={(result) => result.target}
                              columns={[
                                {
                                  id: 'target',
                                  header: '目标',
                                  accessorKey: 'target',
                                  minWidth: 240,
                                  isRowHeader: true,
                                },
                                {
                                  id: 'status',
                                  header: '状态',
                                  cell: (result) => (
                                    <StatusChip status={result.status} />
                                  ),
                                },
                                {
                                  id: 'bytes',
                                  header: '流量',
                                  cell: (result) => formatBytes(result.bytes),
                                },
                                {
                                  id: 'error',
                                  header: '错误',
                                  cell: (result) => result.error || '—',
                                  minWidth: 180,
                                },
                              ]}
                            />
                          )}
                        </div>
                      ))}
                    </section>
                  )}
                </>
              ) : (
                <form
                  onSubmit={(event) => {
                    event.preventDefault()
                    submit()
                  }}
                  className='space-y-4'
                >
                  {tab === 'purge' && (
                    <Choice
                      label='刷新范围'
                      value={mode}
                      onChange={(value) =>
                        setMode(value as CacheJobInput['mode'])
                      }
                      items={[
                        { id: 'url', label: '指定 URL' },
                        { id: 'prefix', label: '目录前缀' },
                        { id: 'all', label: '全部缓存' },
                      ]}
                    />
                  )}
                  {(tab === 'preheat' || mode !== 'all') && (
                    <TextAreaField
                      label='目标 URL'
                      value={targets}
                      onChange={setTargets}
                      rows={6}
                      hint='每行一个 HTTP/HTTPS URL，域名必须属于此网站。'
                    />
                  )}
                  {tab === 'preheat' && (
                    <div className='grid gap-4 sm:grid-cols-2'>
                      <Field
                        type='number'
                        label='每节点并发数'
                        value={concurrency}
                        onChange={setConcurrency}
                        min={1}
                        max={4}
                      />
                      <Field
                        type='number'
                        label='每节点速率（MiB/s）'
                        value={rate}
                        onChange={setRate}
                        min={1}
                        max={100}
                      />
                    </div>
                  )}
                  <div className='flex justify-end gap-2'>
                    <Button size='sm' variant='tertiary' onPress={onClose}>
                      关闭
                    </Button>
                    <Button
                      size='sm'
                      type='submit'
                      isPending={create.isPending}
                    >
                      {tab === 'preheat' ? '提交预热' : '提交刷新'}
                    </Button>
                  </div>
                </form>
              )}
            </Tabs.Panel>
          </Tabs>
        </div>
      </Dialog>
      {confirmation && (
        <Confirm
          title='确认刷新缓存'
          description={`网站「${website.config.name || website.access_domain}」：${confirmation.mode === 'all' ? '清除全部缓存' : `刷新 ${confirmation.targets.length} 个${confirmation.mode === 'prefix' ? '目录' : 'URL'}`}。下一次访问将重新请求源站。`}
          busy={create.isPending}
          onClose={() => setConfirmation(null)}
          onConfirm={() => create.mutateAsync(confirmation)}
        />
      )}
      {cancelTarget && (
        <Confirm
          title='取消缓存任务'
          description={`取消任务 ${cancelTarget.id.slice(0, 8)}，节点已完成的结果将保留。`}
          busy={cancel.isPending}
          onClose={() => setCancelTarget(null)}
          onConfirm={() => cancel.mutateAsync(cancelTarget)}
        />
      )}
    </>
  )
}
