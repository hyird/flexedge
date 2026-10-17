import { useState } from 'react'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { getRouteApi, useNavigate } from '@tanstack/react-router'
import { ArrowDown, ArrowUp, Plus, TrashBin } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Switch } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { fieldErrors } from '@/lib/form-errors'
import { useRevisionDraft } from '@/lib/revision-draft'
import { describeStatus, enablementStatuses } from '@/lib/status'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTable } from '@/components/data-table'
import { Notice } from '@/components/forms'
import { LinkButton } from '@/components/link-button'
import { Page, QueryNotice } from '@/components/page'
import { PageTabs } from '@/components/page-tabs'
import { RevisionConflictAlert } from '@/components/revision-conflict-alert'
import { SaveBar } from '@/components/save-bar'
import { Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { CachePolicyFields } from './cache_policy_fields'
import { CachePolicyWebsites } from './cache_policy_websites'
import { CacheRuleEditor } from './cache_rule_editor'
import {
  afterCachePolicyWrite,
  cachePolicyQuery,
  removeCachePolicy,
  saveCachePolicy,
} from './data'
import {
  cacheMatchLabels,
  cachePolicyInputSchema,
  cachePolicySchema,
  newCacheRule,
  type CachePolicy,
  type CachePolicyInput,
  type CacheRule,
} from './types'

const route = getRouteApi('/_authenticated/cache/$policyId')
export function CachePolicyDetail() {
  const { policyId } = route.useParams()
  const query = useQuery(cachePolicyQuery(policyId))
  if (!query.data)
    return (
      <Page title='缓存策略'>
        <QueryNotice
          query={query}
          title='缓存策略加载失败'
          onRetry={() => query.refetch()}
        />
        {query.isPending && <p className='text-muted'>正在加载缓存策略…</p>}
      </Page>
    )
  return (
    <PolicyEditor
      policy={query.data}
      reload={async () => (await query.refetch()).data}
    />
  )
}
function inputOf(policy: CachePolicy): CachePolicyInput {
  return {
    name: policy.name,
    description: policy.description,
    status: policy.status,
    config: structuredClone(policy.config),
  }
}
function PolicyEditor({
  policy,
  reload,
}: {
  policy: CachePolicy
  reload: () => Promise<CachePolicy | undefined>
}) {
  const client = useQueryClient(),
    navigate = useNavigate()
  const [tab, setTab] = useState<'rules' | 'config' | 'websites'>('rules')
  const [edits, setEdits] = useState<CachePolicyInput>()
  const [errors, setErrors] = useState<Record<string, string>>({})
  const [editing, setEditing] = useState<CacheRule>()
  const [removing, setRemoving] = useState(false)
  const draft = useRevisionDraft({
    server: policy,
    save: (values: CachePolicyInput, revision) =>
      saveCachePolicy(values, {
        id: policy.id,
        revision: revision ?? policy.revision,
      }),
    onSaved: (response) => {
      if (response.data) {
        const updated = cachePolicySchema.parse(response.data)
        client.setQueryData(cachePolicyQuery(policy.id).queryKey, updated)
      }
      setEdits(undefined)
      setErrors({})
      toastWriteSuccess('策略已保存，引用网站的配置将重新分发')
      void afterCachePolicyWrite(client)
    },
    load: () => {
      setEdits(undefined)
      setErrors({})
    },
    reload,
  })
  const base = draft.base ?? policy,
    clean = inputOf(base),
    value = edits ?? clean
  const dirty = JSON.stringify(value) !== JSON.stringify(clean)
  const change = (next: CachePolicyInput) => {
    setEdits(next)
    draft.track(JSON.stringify(next) !== JSON.stringify(clean))
  }
  const changeRules = (rules: CacheRule[]) =>
    change({ ...value, config: { ...value.config, rules } })
  const move = (index: number, offset: number) => {
    const rules = [...value.config.rules]
    const target = index + offset
    if (target < 0 || target >= rules.length) return
    ;[rules[index], rules[target]] = [rules[target], rules[index]]
    changeRules(rules)
  }
  const save = () => {
    const result = cachePolicyInputSchema.safeParse(value)
    if (!result.success) {
      setErrors(fieldErrors(result.error.issues))
      return
    }
    setErrors({})
    draft.save(result.data)
  }
  const columns: DataGridColumn<CacheRule>[] = [
    {
      id: 'order',
      header: '顺序',
      cell: (rule) => value.config.rules.indexOf(rule) + 1,
    },
    {
      id: 'name',
      header: '规则名称',
      isRowHeader: true,
      cell: (rule) => <span>{rule.name}</span>,
    },
    {
      id: 'enabled',
      header: '状态',
      cell: (rule) => (
        <Switch
          size='sm'
          aria-label={'启用规则 ' + rule.name}
          isSelected={rule.enabled}
          onChange={(enabled) =>
            changeRules(
              value.config.rules.map((item) =>
                item.id === rule.id ? { ...item, enabled } : item
              )
            )
          }
        >
          <Switch.Control>
            <Switch.Thumb />
          </Switch.Control>
        </Switch>
      ),
    },
    {
      id: 'match',
      header: '匹配条件',
      cell: (rule) => (
        <span
          className='block max-w-72 truncate'
          title={rule.patterns.join(', ')}
        >
          {cacheMatchLabels[rule.match_type]}
          {rule.patterns.length ? '：' + rule.patterns.join(', ') : ''}
        </span>
      ),
    },
    {
      id: 'action',
      header: '行为',
      cell: (rule) => (rule.action === 'cache' ? '缓存' : '绕过'),
    },
    {
      id: 'ttl',
      header: '缓存时间',
      cell: (rule) =>
        rule.action === 'bypass'
          ? '—'
          : rule.ttl_mode === 'origin'
            ? '遵循源站'
            : rule.ttl_seconds + ' 秒',
    },
    {
      id: 'actions',
      header: '操作',
      cell: (rule) => {
        const index = value.config.rules.indexOf(rule)
        return (
          <div className='flex gap-1'>
            <Button
              size='sm'
              variant='tertiary'
              className='px-2'
              aria-label={`编辑规则${rule.name}`}
              onPress={() => setEditing(rule)}
            >
              编辑
            </Button>
            <Button
              isIconOnly
              size='sm'
              variant='tertiary'
              aria-label={'上移 ' + rule.name}
              isDisabled={index === 0}
              onPress={() => move(index, -1)}
            >
              <ArrowUp />
            </Button>
            <Button
              isIconOnly
              size='sm'
              variant='tertiary'
              aria-label={'下移 ' + rule.name}
              isDisabled={index === value.config.rules.length - 1}
              onPress={() => move(index, 1)}
            >
              <ArrowDown />
            </Button>
            <Button
              isIconOnly
              size='sm'
              variant='tertiary'
              aria-label={'删除规则 ' + rule.name}
              onPress={() =>
                changeRules(
                  value.config.rules.filter((item) => item.id !== rule.id)
                )
              }
            >
              <TrashBin />
            </Button>
          </div>
        )
      },
    },
  ]
  return (
    <Page
      title={base.name}
      description={
        base.is_builtin
          ? '系统内置策略，支持编辑缓存配置，不允许改名或删除。'
          : '网站直接应用此缓存策略，保存后统一分发。'
      }
      actions={
        <>
          <StatusChip {...describeStatus(enablementStatuses, base.status)} />
          <LinkButton href='/cache' state={{ cache: { tab: 'policies' } }}>
            返回缓存中心
          </LinkButton>
          {!base.is_builtin && (
            <Button
              size='sm'
              variant='danger-soft'
              isDisabled={dirty || draft.isSaving}
              onPress={() => setRemoving(true)}
            >
              <TrashBin />
              删除策略
            </Button>
          )}
        </>
      }
    >
      <RevisionConflictAlert draft={draft} />
      <PageTabs
        label='缓存策略详情'
        items={[
          { id: 'rules', label: '缓存规则' },
          { id: 'config', label: '基本配置' },
          { id: 'websites', label: '引用网站（' + base.website_count + '）' },
        ]}
        selected={tab}
        onSelectionChange={setTab}
      >
        {tab === 'rules' ? (
          <Section
            title='缓存规则'
            description='从上到下匹配，首条命中生效；没有匹配规则时绕过缓存。'
            actions={
              <Button
                size='sm'
                isDisabled={value.config.rules.length >= 128}
                onPress={() => setEditing(newCacheRule())}
              >
                <Plus />
                添加规则
              </Button>
            }
          >
            <DataTable
              label='缓存规则'
              columns={columns}
              data={value.config.rules}
              isLoading={false}
              variant='secondary'
              empty={{
                title: '尚未添加缓存规则',
                description: '添加缓存条件后，匹配的内容才会缓存。',
              }}
            />
            {Object.entries(errors)
              .filter(([key]) => key.startsWith('config.rules'))
              .map(([key, error]) => (
                <p key={key} className='text-sm text-danger'>
                  {error}
                </p>
              ))}
          </Section>
        ) : tab === 'config' ? (
          <CachePolicyFields
            value={value}
            onChange={change}
            errors={errors}
            isBuiltin={base.is_builtin}
          />
        ) : (
          <CachePolicyWebsites policyId={base.id} />
        )}
      </PageTabs>
      {dirty && base.website_count > 0 && (
        <Notice>保存后将更新 {base.website_count} 个引用网站的配置。</Notice>
      )}
      <SaveBar
        isDirty={dirty}
        isSaving={draft.isSaving}
        onSave={save}
        onDiscard={draft.discard}
        error={
          draft.error ? apiErrorMessage(draft.error) : Object.values(errors)[0]
        }
      />
      {editing && (
        <CacheRuleEditor
          key={editing.id}
          rule={editing}
          onClose={() => setEditing(undefined)}
          onSave={(rule) => {
            const existing = value.config.rules.some(
              (item) => item.id === rule.id
            )
            changeRules(
              existing
                ? value.config.rules.map((item) =>
                    item.id === rule.id ? rule : item
                  )
                : [...value.config.rules, rule]
            )
            setEditing(undefined)
          }}
        />
      )}
      <ConfirmDialog
        isOpen={removing}
        onClose={() => setRemoving(false)}
        title={'删除策略 ' + base.name + '？'}
        description={
          base.website_count
            ? '当前仍有 ' +
              base.website_count +
              ' 个网站引用此策略，请先解除引用。'
            : '删除后此策略不可恢复。'
        }
        confirmLabel='删除策略'
        onConfirm={async () => {
          await removeCachePolicy(base)
          toastWriteSuccess('缓存策略已删除')
          await navigate({
            to: '/cache',
            state: { cache: { tab: 'policies' } },
            replace: true,
            ignoreBlocker: true,
          })
          void afterCachePolicyWrite(client)
        }}
      />
    </Page>
  )
}
