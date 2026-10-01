import { useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { DataGrid, type DataGridColumn } from '@heroui-pro/react'
import { Button, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import { Confirm } from '@/components/confirm'
import { Drawer } from '@/components/drawer'
import {
  Choice,
  Field,
  FormActions,
  Notice,
  TextAreaField,
} from '@/components/forms'
import { RowMenu } from '@/components/row-menu'
import { StatusChip } from '@/components/status-chip'
import { createCachePolicy, updateCachePolicy } from './data'
import {
  cachePolicySchema,
  formatDuration,
  type CachePolicyFormValues,
} from './form'
import { CacheRuleEditor } from './rule-editor'
import type { CachePolicy, CacheRule } from './types'
import {
  cloneCacheRule,
  createCacheRule,
  createDefaultCacheRule,
  isDefaultCacheRule,
} from './utils'

export function CachePolicyDialog({
  policy,
  overview = false,
  onClose,
}: {
  policy?: CachePolicy
  overview?: boolean
  onClose: () => void
}) {
  const client = useQueryClient()
  const [editing, setEditing] = useState(!policy || !overview)
  const [values, setValues] = useState<CachePolicyFormValues>({
    name: policy?.name ?? '',
    description: policy?.description ?? '',
    status: policy?.status ?? 'enabled',
    rules: policy?.rules.map(cloneCacheRule) ?? [createDefaultCacheRule()],
  })
  const [errors, setErrors] = useState<Record<string, string>>({})
  const [rule, setRule] = useState<CacheRule | null>(null)
  const [remove, setRemove] = useState<CacheRule | null>(null)
  const mutation = useMutation({
    mutationFn: (input: CachePolicyFormValues) =>
      policy ? updateCachePolicy(policy, input) : createCachePolicy(input),
    onSuccess: (response) => {
      toast.success(response.message || '已保存')
      void client.invalidateQueries({ queryKey: queryKeys.cachePolicies })
      void client.invalidateQueries({ queryKey: queryKeys.websites })
      onClose()
    },
  })
  const move = (current: CacheRule, offset: -1 | 1) =>
    setValues((draft) => {
      const from = draft.rules.findIndex((item) => item.id === current.id)
      const to = from + offset
      if (
        to < 0 ||
        to >= draft.rules.length ||
        isDefaultCacheRule(draft.rules[from]) ||
        isDefaultCacheRule(draft.rules[to])
      )
        return draft
      const rules = [...draft.rules]
      ;[rules[from], rules[to]] = [rules[to], rules[from]]
      return { ...draft, rules }
    })
  const applyRule = (next: CacheRule) => {
    setValues((draft) => {
      const index = draft.rules.findIndex((item) => item.id === next.id)
      const rules = [...draft.rules]
      if (index < 0) rules.splice(Math.max(0, rules.length - 1), 0, next)
      else rules[index] = next
      return { ...draft, rules }
    })
    setRule(null)
  }
  const columns: DataGridColumn<CacheRule>[] = [
    {
      id: 'name',
      header: '条件',
      isRowHeader: true,
      minWidth: 180,
      cell: (item) => (
        <div>
          <strong className='font-medium'>{item.name}</strong>
          <p className='mt-1 text-xs text-muted'>
            {isDefaultCacheRule(item)
              ? '默认规则 · 始终最后执行'
              : item.match_type === 'all'
                ? '全部请求'
                : item.patterns.join('、')}
          </p>
        </div>
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 90,
      cell: (item) => <StatusChip status={item.status} />,
    },
    {
      id: 'action',
      header: '行为',
      minWidth: 90,
      cell: (item) => (item.action === 'cache' ? '缓存' : '绕过'),
    },
    {
      id: 'ttl',
      header: 'TTL',
      minWidth: 100,
      cell: (item) =>
        item.action === 'bypass'
          ? '—'
          : item.ttl_seconds
            ? formatDuration(item.ttl_seconds)
            : item.ignore_origin_cache_control
              ? '复用前验证'
              : '遵循源站时长',
    },
    ...(editing
      ? [
          {
            id: 'actions',
            header: '操作',
            width: 70,
            cell: (item: CacheRule) => (
              <RowMenu
                label={`${item.name}条件操作`}
                actions={[
                  {
                    id: 'edit',
                    label: '编辑条件',
                    disabled: mutation.isPending,
                    onAction: () => setRule(cloneCacheRule(item)),
                  },
                  ...(!isDefaultCacheRule(item)
                    ? [
                        {
                          id: 'up',
                          label: '上移',
                          disabled:
                            mutation.isPending ||
                            values.rules[0].id === item.id,
                          onAction: () => move(item, -1),
                        },
                        {
                          id: 'down',
                          label: '下移',
                          disabled:
                            mutation.isPending ||
                            values.rules.at(-2)?.id === item.id,
                          onAction: () => move(item, 1),
                        },
                        {
                          id: 'delete',
                          label: '删除条件',
                          danger: true,
                          disabled: mutation.isPending,
                          onAction: () => setRemove(item),
                        },
                      ]
                    : []),
                ]}
              />
            ),
          },
        ]
      : []),
  ]
  return (
    <>
      <Drawer
        title={
          rule
            ? `编辑条件 · ${rule.name}`
            : policy
              ? `缓存策略 · ${policy.name}`
              : '新建缓存策略'
        }
        onClose={() => {
          if (rule) {
            toast.warning('请先应用或取消当前条件草稿。')
            return
          }
          onClose()
        }}
        busy={mutation.isPending}
        size='lg'
      >
        {rule ? (
          <CacheRuleEditor
            key={rule.id}
            rule={rule}
            onCancel={() => setRule(null)}
            onApply={applyRule}
          />
        ) : (
          <form
            className='grid gap-5'
            onSubmit={(event) => {
              event.preventDefault()
              if (!editing || mutation.isPending) return
              const result = cachePolicySchema.safeParse(values)
              if (!result.success) {
                setErrors(
                  Object.fromEntries(
                    result.error.issues.map((issue) => [
                      String(issue.path[0]),
                      issue.message,
                    ])
                  )
                )
                return
              }
              setErrors({})
              mutation.mutate(result.data)
            }}
          >
            {policy?.website_count ? (
              <p className='text-sm text-muted'>
                保存将影响 {policy.website_count} 个关联网站。
              </p>
            ) : null}
            {editing ? (
              <fieldset
                disabled={mutation.isPending}
                className='grid min-w-0 gap-4'
              >
                <div className='grid gap-3 sm:grid-cols-2'>
                  <Field
                    label='策略名称'
                    value={values.name}
                    onChange={(name) =>
                      setValues((draft) => ({ ...draft, name }))
                    }
                    required
                    error={errors.name}
                  />
                  <Choice
                    label='状态'
                    value={values.status}
                    onChange={(status) =>
                      setValues((draft) => ({
                        ...draft,
                        status: status as CachePolicy['status'],
                      }))
                    }
                    items={[
                      { id: 'enabled', label: '启用' },
                      { id: 'disabled', label: '停用' },
                    ]}
                  />
                </div>
                <TextAreaField
                  label='描述'
                  rows={2}
                  value={values.description}
                  onChange={(description) =>
                    setValues((draft) => ({ ...draft, description }))
                  }
                  error={errors.description}
                />
              </fieldset>
            ) : (
              <>
                <div className='flex items-center gap-3'>
                  <StatusChip status={values.status} />
                  <span className='text-xs text-muted'>
                    {values.rules.length} 条缓存条件 ·{' '}
                    {policy?.website_count ?? 0} 个关联网站
                  </span>
                </div>
                <p className='text-sm whitespace-pre-wrap text-muted'>
                  {values.description || '暂无描述'}
                </p>
              </>
            )}
            <p className='text-sm text-muted'>
              按顺序判断，第一条启用且匹配的规则生效；未匹配时使用末尾默认规则，默认遵守源站缓存指令。
            </p>
            <section className='grid gap-3'>
              <div className='flex flex-wrap items-center justify-between gap-2'>
                <h3 className='text-sm font-medium'>缓存条件</h3>
                {editing && (
                  <div className='flex gap-2'>
                    <Button
                      size='sm'
                      variant='secondary'
                      isDisabled={
                        mutation.isPending || values.rules.length >= 65
                      }
                      onPress={() => setRule(createCacheRule('cache'))}
                    >
                      添加缓存条件
                    </Button>
                    <Button
                      size='sm'
                      variant='ghost'
                      isDisabled={
                        mutation.isPending || values.rules.length >= 65
                      }
                      onPress={() => setRule(createCacheRule('bypass'))}
                    >
                      添加绕过条件
                    </Button>
                  </div>
                )}
              </div>
              <DataGrid
                aria-label='缓存条件'
                data={values.rules}
                columns={columns}
                getRowId={(item) => item.id}
              />
            </section>
            {!editing && !!policy?.websites.length && (
              <section className='grid gap-2'>
                <h3 className='text-sm font-medium'>关联网站</h3>
                <p className='text-sm text-muted'>
                  {policy.websites.map((website) => website.name).join('、')}
                </p>
              </section>
            )}
            {errors.rules && <Notice>{errors.rules}</Notice>}
            {mutation.isError && (
              <Notice>{apiErrorMessage(mutation.error)}</Notice>
            )}
            {editing ? (
              <FormActions
                onCancel={onClose}
                busy={mutation.isPending}
                label='保存策略'
              />
            ) : (
              <div className='flex justify-end gap-2'>
                <Button size='sm' variant='secondary' onPress={onClose}>
                  关闭
                </Button>
                <Button size='sm' onPress={() => setEditing(true)}>
                  编辑策略
                </Button>
              </div>
            )}
          </form>
        )}
      </Drawer>
      {remove && (
        <Confirm
          title='删除缓存条件'
          description={`将从策略草稿移除“${remove.name}”，保存策略后生效。`}
          onClose={() => setRemove(null)}
          onConfirm={() => {
            setValues((draft) => ({
              ...draft,
              rules: draft.rules.filter((item) => item.id !== remove.id),
            }))
            setRemove(null)
          }}
        />
      )}
    </>
  )
}
