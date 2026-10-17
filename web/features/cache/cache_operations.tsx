import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { fieldErrors } from '@/lib/form-errors'
import { formatBytes, formatDate } from '@/lib/format'
import { describeStatus } from '@/lib/status'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTable, TruncatedText } from '@/components/data-table'
import { FormSheet } from '@/components/form-sheet'
import { Choice, Notice, TextAreaField } from '@/components/forms'
import { LinkButton } from '@/components/link-button'
import { QueryNotice } from '@/components/page'
import { Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import {
  taskHref,
  taskStatuses,
  taskStatusReserve,
} from '@/features/tasks/task-display'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import {
  cacheOperationsQuery,
  cachePolicyOptionsQuery,
  cacheWebsitesQuery,
  submitCacheOperation,
  retryCacheOperation,
} from './data'
import {
  cacheOperationInputSchema,
  type CacheOperation,
  type CacheOperationInput,
  type CacheOperationKind,
} from './types'

const operationLabels = {
  prewarm: '预热',
  refresh: '刷新',
  clean: '清理缓存',
} as const
const scopeLabels = {
  urls: 'URL',
  prefix: '目录',
  all: '全部内容',
  expired: '过期内容',
} as const
export function CacheOperations({
  operation,
}: {
  operation: CacheOperationKind
}) {
  const client = useQueryClient()
  const sites = useQuery({
    ...cacheWebsitesQuery(),
    enabled: operation !== 'clean',
  })
  const policies = useQuery({
    ...cachePolicyOptionsQuery,
    enabled: operation === 'clean',
  })
  const recent = useQuery(cacheOperationsQuery(operation))
  const [objectId, setObjectId] = useState('')
  const [scope, setScope] = useState<CacheOperationInput['scope']>(
    operation === 'clean' ? 'expired' : 'urls'
  )
  const [urls, setUrls] = useState('')
  const [errors, setErrors] = useState<Record<string, string>>({})
  const [confirmation, setConfirmation] = useState<CacheOperationInput>()
  const [detail, setDetail] = useState<CacheOperation>()
  const options =
    operation === 'clean'
      ? (policies.data ?? []).map((item) => ({ id: item.id, label: item.name }))
      : (sites.data ?? [])
          .filter(
            (item) => operation !== 'prewarm' || item.status === 'enabled'
          )
          .map((item) => ({
            id: item.id,
            label: item.name + (item.domains[0] ? ' · ' + item.domains[0] : ''),
            textValue: item.name + ' ' + item.domains.join(' '),
          }))
  const needsUrls = scope === 'urls' || scope === 'prefix'
  const mutation = useMutation({
    meta: { inlineError: true },
    mutationFn: submitCacheOperation,
    onSuccess: (response) => {
      if (response.data)
        toastWriteSuccess(
          operationLabels[operation] +
            '任务已提交，共 ' +
            response.data.target_node_count +
            ' 个节点',
          { resourceType: 'cache', resourceId: response.data.id }
        )
      void client.invalidateQueries({ queryKey: ['tasks'] })
    },
  })
  const retry = useMutation({
    meta: { inlineError: true },
    mutationFn: retryCacheOperation,
    onSuccess: () => toastWriteSuccess('任务已重新排队'),
  })
  const submit = () => {
    const result = cacheOperationInputSchema.safeParse({
      operation,
      object_id: objectId,
      scope,
      urls: needsUrls
        ? [
            ...new Set(
              urls
                .split(/\r?\n/)
                .map((value) => value.trim())
                .filter(Boolean)
            ),
          ]
        : [],
    })
    if (!result.success) {
      setErrors(fieldErrors(result.error.issues))
      return
    }
    setErrors({})
    if (operation === 'clean' || scope === 'all') setConfirmation(result.data)
    else mutation.mutate(result.data)
  }
  const activeDetail = detail
    ? (recent.data?.find((item) => item.id === detail.id) ?? detail)
    : undefined
  const columns: DataGridColumn<CacheOperation>[] = [
    {
      id: 'name',
      header: operation === 'clean' ? '缓存策略' : '网站',
      isRowHeader: true,
      cell: (item) => <span>{item.name || '已删除的资源'}</span>,
    },
    {
      id: 'scope',
      header: '范围',
      cell: (item) =>
        scopeLabels[item.scope as keyof typeof scopeLabels] ?? item.scope,
    },
    {
      id: 'status',
      header: '状态',
      cell: (item) => (
        <StatusChip
          {...describeStatus(taskStatuses, item.status)}
          reserve={taskStatusReserve}
        />
      ),
    },
    {
      id: 'progress',
      header: '完成节点',
      cell: (item) =>
        item.targets.filter((target) => target.status === 'completed').length +
        ' / ' +
        item.targets.length,
    },
    {
      id: 'updated',
      header: '更新时间',
      cell: (item) => formatDate(item.updated_at),
    },
    {
      id: 'error',
      header: '执行信息',
      cell: (item) => <TruncatedText text={item.error} />,
    },
    {
      id: 'actions',
      header: '操作',
      cell: (item) => (
        <div className='flex gap-1'>
          <Button
            size='sm'
            variant='tertiary'
            className='px-2'
            aria-label={`查看执行详情${item.name || '已删除的资源'}`}
            onPress={() => setDetail(item)}
          >
            详情
          </Button>
          <LinkButton
            href={taskHref({ resourceType: 'cache', resourceId: item.id })}
            variant='tertiary'
          >
            查看任务
          </LinkButton>
          {(item.status === 'failed' || item.status === 'retrying') && (
            <Button
              size='sm'
              variant='tertiary'
              isPending={retry.isPending && retry.variables === item.id}
              onPress={() => retry.mutate(item.id)}
            >
              重试
            </Button>
          )}
        </div>
      ),
    },
  ]
  return (
    <div className='flex flex-col gap-6'>
      <Section
        title={operationLabels[operation]}
        description={
          operation === 'prewarm'
            ? '提前把指定URL的内容加载到网站所在节点的缓存中。'
            : operation === 'refresh'
              ? '使指定内容失效，后续访问时重新回源。'
              : '按策略清理节点缓存，释放过期或全部缓存内容占用的空间。'
        }
      >
        <form
          className='flex flex-col gap-4'
          noValidate
          onSubmit={(event) => {
            event.preventDefault()
            submit()
          }}
        >
          <QueryNotice
            query={operation === 'clean' ? policies : sites}
            title='可用资源加载失败'
            onRetry={() =>
              operation === 'clean' ? policies.refetch() : sites.refetch()
            }
          />
          <div className='grid gap-4 sm:grid-cols-2'>
            <Choice
              label={operation === 'clean' ? '缓存策略' : '网站'}
              value={objectId}
              onChange={setObjectId}
              items={options}
              error={errors.object_id}
              hint={
                operation === 'clean'
                  ? '清理所有节点中属于所选策略的缓存。'
                  : '仅显示已启用缓存的网站。'
              }
            />
            {operation !== 'prewarm' && (
              <Choice
                label='操作范围'
                value={scope}
                onChange={(value) => setScope(value as typeof scope)}
                items={
                  operation === 'clean'
                    ? [
                        { id: 'expired', label: '过期内容' },
                        { id: 'all', label: '全部内容' },
                      ]
                    : [
                        { id: 'urls', label: '指定URL' },
                        { id: 'prefix', label: '目录' },
                        { id: 'all', label: '整站' },
                      ]
                }
                error={errors.scope}
              />
            )}
          </div>
          {!options.length &&
            !(operation === 'clean' ? policies : sites).isPending && (
              <Notice>
                {operation === 'clean'
                  ? '请先创建缓存策略。'
                  : '请先在网站配置中启用缓存并选择策略。'}
              </Notice>
            )}
          {needsUrls && (
            <TextAreaField
              label={scope === 'prefix' ? '目录列表' : 'URL列表'}
              value={urls}
              onChange={setUrls}
              rows={7}
              hint={
                scope === 'prefix'
                  ? '每行一个完整URL目录，例如https://example.com/static/，最多100项。'
                  : '每行一个完整HTTP/HTTPS URL，最多100项，域名须属于所选网站。'
              }
              error={errors.urls}
            />
          )}
          {operation === 'clean' && objectId && (
            <Notice>
              此策略被{' '}
              {policies.data?.find((item) => item.id === objectId)
                ?.website_count ?? 0}{' '}
              个网站引用，操作将发送到全部相关节点。
            </Notice>
          )}
          {mutation.isError && (
            <Notice>{apiErrorMessage(mutation.error)}</Notice>
          )}
          {mutation.data?.data && (
            <Notice success>
              任务已提交，共 {mutation.data.data.target_node_count} 个目标节点。
              <LinkButton
                href={taskHref({
                  resourceType: 'cache',
                  resourceId: mutation.data.data.id,
                })}
                variant='tertiary'
              >
                查看任务
              </LinkButton>
            </Notice>
          )}
          <div>
            <Button
              type='submit'
              size='sm'
              isPending={mutation.isPending}
              isDisabled={!objectId}
            >
              提交{operationLabels[operation]}
            </Button>
          </div>
        </form>
      </Section>
      <Section
        title='最近任务'
        description='同一对象的同类操作复用同一条任务，执行历史保留在任务中。'
      >
        {retry.error && <Notice>{apiErrorMessage(retry.error)}</Notice>}
        <DataTable
          label='缓存操作任务'
          columns={columns}
          data={recent.data}
          isLoading={recent.isPending}
          error={recent.isError ? recent.error : undefined}
          onRetry={() => recent.refetch()}
          variant='secondary'
          empty={{ title: '暂无' + operationLabels[operation] + '任务' }}
        />
      </Section>
      <ConfirmDialog
        isOpen={!!confirmation}
        onClose={() => setConfirmation(undefined)}
        title={operationLabels[operation] + '？'}
        description={
          scope === 'expired'
            ? '将移除所选策略的过期缓存，正在使用的有效内容继续保留。'
            : '将移除所选范围的缓存内容，后续访问需要重新回源。'
        }
        confirmLabel={'确认' + operationLabels[operation]}
        onConfirm={async () => {
          if (confirmation) await mutation.mutateAsync(confirmation)
        }}
      />
      {activeDetail && (
        <FormSheet
          isOpen
          title={operationLabels[operation] + '执行结果'}
          description={activeDetail.name}
          onClose={() => setDetail(undefined)}
          size='lg'
        >
          {activeDetail.targets.map((target) => (
            <Section
              key={target.node_id}
              title={target.node_name}
              actions={
                <StatusChip
                  {...describeStatus(
                    taskStatuses,
                    target.status === 'pending'
                      ? 'queued'
                      : target.status === 'sent'
                        ? 'running'
                        : target.status
                  )}
                  reserve={taskStatusReserve}
                />
              }
            >
              <p className='text-sm text-muted'>
                完成URL {target.completed_urls} · 处理对象{' '}
                {target.affected_objects} · {formatBytes(target.affected_bytes)}
              </p>
              {target.error && (
                <p className='mt-2 text-sm text-danger'>{target.error}</p>
              )}
              {target.failures.length > 0 && (
                <ul className='mt-2 space-y-1 text-sm text-danger'>
                  {target.failures.map((failure, index) => (
                    <li className='break-all' key={index}>
                      {failure}
                    </li>
                  ))}
                </ul>
              )}
            </Section>
          ))}
        </FormSheet>
      )}
    </div>
  )
}
