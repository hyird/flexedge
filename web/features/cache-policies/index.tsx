import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { formatDate } from '@/lib/format'
import { queryKeys } from '@/lib/query-keys'
import { Confirm } from '@/components/confirm'
import { Choice, Dialog, Field, FormActions, Notice } from '@/components/forms'
import { Page } from '@/components/page'
import { ResourceList, ResourceSearch } from '@/components/resource-list'
import { RowMenu } from '@/components/row-menu'
import { StatusChip } from '@/components/status-chip'
import {
  cachePoliciesQuery,
  copyCachePolicy,
  removeCachePolicy,
  updateCachePolicy,
} from './data'
import { CachePolicyDialog } from './dialog'
import type { CachePolicy } from './types'

export function CachePoliciesPage() {
  const client = useQueryClient()
  const [page, setPage] = useState(1)
  const [size, setSize] = useState(20)
  const [draft, setDraft] = useState('')
  const [keyword, setKeyword] = useState('')
  const [status, setStatus] = useState('all')
  const [dialog, setDialog] = useState<{
    policy?: CachePolicy
    overview?: boolean
  } | null>(null)
  const [target, setTarget] = useState<{
    policy: CachePolicy
    action: 'remove' | 'toggle'
  } | null>(null)
  const [copy, setCopy] = useState<CachePolicy | null>(null)
  const query = useQuery(
    cachePoliciesQuery({
      page,
      page_size: size,
      keyword: keyword || undefined,
      status: status === 'all' ? undefined : status,
    })
  )
  const mutation = useMutation({
    mutationFn: (value: NonNullable<typeof target>) =>
      value.action === 'remove'
        ? removeCachePolicy(value.policy)
        : updateCachePolicy(value.policy, {
            name: value.policy.name,
            description: value.policy.description,
            status: value.policy.status === 'enabled' ? 'disabled' : 'enabled',
            rules: value.policy.rules,
          }),
    onSuccess: (response) => {
      toast.success(response.message || '已更新')
      setTarget(null)
      void client.invalidateQueries({ queryKey: queryKeys.cachePolicies })
      void client.invalidateQueries({ queryKey: queryKeys.websites })
    },
    onError: (error) => toast.danger(apiErrorMessage(error)),
  })
  const columns: DataGridColumn<CachePolicy>[] = [
    {
      id: 'name',
      header: '策略名称',
      isRowHeader: true,
      minWidth: 230,
      cell: (item) => (
        <div>
          <Button
            size='sm'
            variant='ghost'
            className='justify-start px-0 font-medium'
            onPress={() => setDialog({ policy: item, overview: true })}
          >
            {item.name}
          </Button>
          <p className='max-w-72 truncate text-xs text-muted'>
            {item.description || '暂无描述'}
          </p>
        </div>
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 100,
      cell: (item) => <StatusChip status={item.status} />,
    },
    {
      id: 'rules',
      header: '缓存条件',
      minWidth: 100,
      cell: (item) => (
        <span className='tabular-nums'>{item.rules.length} 条</span>
      ),
    },
    {
      id: 'websites',
      header: '关联网站',
      minWidth: 100,
      cell: (item) => (
        <span className='tabular-nums'>{item.website_count} 个</span>
      ),
    },
    {
      id: 'updated',
      header: '最近更新',
      minWidth: 170,
      cell: (item) => (
        <span className='text-xs text-muted'>
          {formatDate(item.updated_at)}
        </span>
      ),
    },
    {
      id: 'actions',
      header: '操作',
      width: 70,
      cell: (item) => (
        <RowMenu
          label={`${item.name}策略操作`}
          actions={[
            {
              id: 'detail',
              label: '查看详情',
              onAction: () => setDialog({ policy: item, overview: true }),
            },
            {
              id: 'edit',
              label: '编辑',
              onAction: () => setDialog({ policy: item }),
            },
            { id: 'copy', label: '复制策略', onAction: () => setCopy(item) },
            {
              id: 'toggle',
              label: item.status === 'enabled' ? '停用' : '启用',
              onAction: () => setTarget({ policy: item, action: 'toggle' }),
            },
            {
              id: 'delete',
              label: '删除',
              danger: true,
              onAction: () => setTarget({ policy: item, action: 'remove' }),
            },
          ]}
        />
      ),
    },
  ]
  return (
    <Page
      title='缓存策略'
      description='按请求条件决定边缘缓存行为，将策略复用于多个网站。'
      actions={
        <Button size='sm' onPress={() => setDialog({})}>
          新建策略
        </Button>
      }
    >
      <ResourceList
        label='缓存策略'
        query={query}
        columns={columns}
        page={page}
        onPageChange={setPage}
        pageSize={size}
        onPageSizeChange={(value) => {
          setSize(value)
          setPage(1)
        }}
        onRefresh={() => void query.refetch()}
        filters={
          <form
            className='flex flex-wrap items-end gap-2'
            onSubmit={(event) => {
              event.preventDefault()
              setKeyword(draft.trim())
              setPage(1)
            }}
          >
            <ResourceSearch
              label='搜索策略'
              value={draft}
              onChange={setDraft}
            />
            <Choice
              compact
              label='策略状态'
              value={status}
              onChange={(value) => {
                setStatus(value)
                setPage(1)
              }}
              items={[
                { id: 'all', label: '全部状态' },
                { id: 'enabled', label: '已启用' },
                { id: 'disabled', label: '已停用' },
              ]}
            />
            <Button size='sm' type='submit' variant='secondary'>
              搜索
            </Button>
            <Button
              size='sm'
              variant='ghost'
              onPress={() => {
                setDraft('')
                setKeyword('')
                setStatus('all')
                setPage(1)
              }}
            >
              重置
            </Button>
          </form>
        }
        emptyTitle='暂无缓存策略'
        emptyDescription='创建缓存条件，再将策略关联到网站。'
      />
      {dialog && (
        <CachePolicyDialog
          key={dialog.policy?.id ?? 'new'}
          policy={dialog.policy}
          overview={dialog.overview}
          onClose={() => setDialog(null)}
        />
      )}
      {copy && <CopyPolicyDialog policy={copy} onClose={() => setCopy(null)} />}
      {target && (
        <Confirm
          title={
            target.action === 'remove'
              ? '删除缓存策略'
              : target.policy.status === 'enabled'
                ? '停用缓存策略'
                : '启用缓存策略'
          }
          description={
            target.action === 'remove'
              ? `将删除“${target.policy.name}”。关联网站会阻止此操作。`
              : `“${target.policy.name}”的状态变更将影响 ${target.policy.website_count} 个关联网站。`
          }
          busy={mutation.isPending}
          onClose={() => setTarget(null)}
          onConfirm={() => mutation.mutateAsync(target)}
        />
      )}
    </Page>
  )
}

function CopyPolicyDialog({
  policy,
  onClose,
}: {
  policy: CachePolicy
  onClose: () => void
}) {
  const client = useQueryClient()
  const [name, setName] = useState(`${policy.name} 副本`)
  const mutation = useMutation({
    mutationFn: () => copyCachePolicy(policy, name.trim()),
    onSuccess: (response) => {
      toast.success(response.message || '已复制')
      void client.invalidateQueries({ queryKey: queryKeys.cachePolicies })
      onClose()
    },
  })
  return (
    <Dialog title='复制缓存策略' onClose={onClose} busy={mutation.isPending}>
      <form
        className='grid gap-4'
        onSubmit={(event) => {
          event.preventDefault()
          if (name.trim()) mutation.mutate()
        }}
      >
        <Field
          label='新策略名称'
          value={name}
          onChange={setName}
          required
          disabled={mutation.isPending}
        />
        {mutation.isError && <Notice>{apiErrorMessage(mutation.error)}</Notice>}
        <FormActions
          onCancel={onClose}
          busy={mutation.isPending}
          label='复制'
        />
      </form>
    </Dialog>
  )
}
