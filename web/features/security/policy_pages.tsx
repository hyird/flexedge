import { useState, type ReactNode } from 'react'
import { useParams } from '@tanstack/react-router'
import { ArrowLeft, Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Link, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { DataTable, TruncatedText } from '@/components/data-table'
import { Choice, Field } from '@/components/forms'
import { ListToolbar } from '@/components/list-toolbar'
import { Page } from '@/components/page'
import { PageTabs } from '@/components/page-tabs'
import { SaveBar } from '@/components/save-bar'
import { Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { actions, expression, newRule, stages, type Rule } from './model'
import {
  useSecurityData,
  type RequestSample,
  type SecurityPolicy,
} from './security_data'
import { useSecuritySearch } from './security_navigation'
import { SecurityRuleEditor } from './security_rule_editor'

export function PoliciesPanel() {
  const data = useSecurityData(),
    { search, patch, router } = useSecuritySearch()
  const columns: DataGridColumn<SecurityPolicy>[] = [
    {
      id: 'name',
      header: '规则集名称',
      isRowHeader: true,
      cell: (policy) => (
        <Link
          href={`/security/policies/${policy.id}`}
          className='text-sm font-medium'
        >
          {policy.name}
        </Link>
      ),
    },
    {
      id: 'custom',
      header: '自定义规则',
      cell: (policy) =>
        policy.rules.filter((rule) => rule.stage === 'custom').length,
    },
    {
      id: 'rate',
      header: 'CC / 限速规则',
      cell: (policy) =>
        policy.rules.filter((rule) => rule.stage === 'rate').length,
    },
    {
      id: 'sites',
      header: '应用网站',
      cell: (policy) =>
        data.websites.filter((website) => website.policyId === policy.id)
          .length,
    },
    {
      id: 'description',
      header: '描述',
      cell: (policy) => <TruncatedText text={policy.description} />,
    },
  ]
  return (
    <>
      <ListToolbar
        search={{
          value: search.q,
          onChange: (q) => patch({ q }),
          placeholder: '搜索规则集名称',
        }}
        actions={
          <Button
            size='sm'
            onPress={() => {
              void router.navigate({ href: '/security/policies/new' })
            }}
          >
            <Plus />
            创建规则集
          </Button>
        }
      />
      <DataTable
        label='规则集'
        columns={columns}
        data={data.policies.filter((policy) =>
          `${policy.name} ${policy.description}`.includes(search.q)
        )}
        isLoading={false}
        empty={{ title: '暂无匹配的规则集' }}
      />
    </>
  )
}
export function SecurityPolicyDetail() {
  const data = useSecurityData(),
    { router } = useSecuritySearch(),
    { policyId } = useParams({ strict: false }) as { policyId: string }
  const creating = policyId === 'new',
    [starter] = useState<SecurityPolicy>(() => ({
      id: crypto.randomUUID(),
      name: '',
      description: '',
      rules: [],
      revision: 0,
      managed: { paranoiaLevel: 1, inboundThreshold: 5 },
    }))
  const policy = creating
    ? starter
    : data.policies.find((item) => item.id === policyId)
  const back = () => {
    data.setLandingTab('policies')
    void router.navigate({ href: '/security' })
  }
  if (!policy)
    return (
      <Page
        title='规则集不存在'
        actions={
          <Button size='sm' onPress={back}>
            返回安全中心
          </Button>
        }
      >
        {null}
      </Page>
    )
  const columns: DataGridColumn<Rule>[] = [
    {
      id: 'order',
      header: '顺序',
      cell: (rule) =>
        policy.rules.filter((item) => item.stage === rule.stage).indexOf(rule) +
        1,
    },
    {
      id: 'name',
      header: '规则名称',
      isRowHeader: true,
      cell: (rule) => (
        <Link
          href={`/security/policies/${policy.id}/rules/${rule.id}`}
          className='text-sm font-medium'
        >
          <TruncatedText text={rule.name} />
        </Link>
      ),
    },
    {
      id: 'enabled',
      header: '状态',
      cell: (rule) => (
        <StatusChip
          label={rule.enabled ? '已启用' : '草稿'}
          tone={rule.enabled ? 'success' : 'default'}
          reserve={['已启用', '已停用', '草稿']}
        />
      ),
    },
    {
      id: 'match',
      header: '匹配条件',
      cell: (rule) => (
        <TruncatedText text={expression(rule)} className='max-w-80' />
      ),
    },
    {
      id: 'frequency',
      header: '触发条件',
      cell: (rule) =>
        rule.stage === 'rate'
          ? `${rule.window} 秒内超过 ${rule.limit} 次`
          : '匹配请求',
    },
    {
      id: 'action',
      header: '动作',
      cell: (rule) => (
        <StatusChip
          {...actions[rule.action]}
          reserve={Object.values(actions).map((action) => action.label)}
        />
      ),
    },
    {
      id: 'link',
      header: '入列联动',
      cell: (rule) =>
        rule.enrollment.enabled
          ? data.lists.find((list) => list.id === rule.enrollment.list_id)
              ?.description ||
            data.lists.find((list) => list.id === rule.enrollment.list_id)?.name
          : '—',
    },
  ]
  return (
    <Page
      title={creating ? '创建规则集' : policy.name}
      description={
        creating
          ? undefined
          : `${policy.description} · ${data.websites.filter((website) => website.policyId === policy.id).length} 个网站应用`
      }
      actions={
        <Button size='sm' variant='tertiary' onPress={back}>
          <ArrowLeft />
          返回
        </Button>
      }
    >
      <PolicyConfiguration key={policy.id} policy={policy} creating={creating}>
        {(managedConfiguration, managedSaved, configurationDirty) =>
          !creating && (
            <>
              {stages.map((stage) => (
                <Section
                  key={stage.id}
                  title={`${stage.label}（${policy.rules.filter((rule) => rule.stage === stage.id).length}）`}
                  actions={
                    stage.id !== 'managed' ? (
                      <Button
                        size='sm'
                        onPress={() => {
                          void router.navigate({
                            href: `/security/policies/${policy.id}/rules/new?stage=${stage.id}`,
                          })
                        }}
                      >
                        <Plus />
                        添加
                        {stage.id === 'rate' ? ' CC / 限速规则' : '自定义规则'}
                      </Button>
                    ) : undefined
                  }
                >
                  {stage.id === 'managed' && managedConfiguration}
                  {stage.id === 'managed' ? (
                    <ManagedRulesTable
                      policy={policy}
                      locked={configurationDirty}
                      onSaved={managedSaved}
                    />
                  ) : (
                    <DataTable
                      label={stage.label}
                      columns={columns}
                      data={policy.rules.filter(
                        (rule) => rule.stage === stage.id
                      )}
                      isLoading={false}
                      variant='secondary'
                      empty={{ title: '暂无规则' }}
                    />
                  )}
                </Section>
              ))}
            </>
          )
        }
      </PolicyConfiguration>
    </Page>
  )
}
function ManagedRulesTable({
  policy,
  locked,
  onSaved,
}: {
  policy: SecurityPolicy
  locked: boolean
  onSaved: (revision: number) => void
}) {
  const data = useSecurityData(),
    { search, patch } = useSecuritySearch()
  const [pending, setPending] = useState<number | null>(null),
    [error, setError] = useState('')
  const states = new Map(
    policy.rules
      .filter((rule) => rule.stage === 'managed')
      .map((rule) => [rule.id, rule.enabled])
  )
  const rows = (data.core.data?.catalog ?? [])
    .map((rule) => ({ ...rule, enabled: states.get(String(rule.id)) ?? false }))
    .filter((rule) =>
      `${rule.id} ${rule.name} ${rule.category} PL${rule.paranoia_level}`
        .toLowerCase()
        .includes(search.q.toLowerCase())
    )
  const toggle = async (row: (typeof rows)[number]) => {
    if (pending !== null || locked) return
    setPending(row.id)
    setError('')
    try {
      const saved = await data.setPolicies(
        (policies) =>
          policies.map((item) =>
            item.id === policy.id
              ? {
                  ...item,
                  rules: item.rules.map((rule) =>
                    rule.stage === 'managed' && rule.id === String(row.id)
                      ? { ...rule, enabled: !row.enabled }
                      : rule
                  ),
                }
              : item
          ),
        policy.revision
      )
      onSaved(saved.revision)
      toast.success(`规则 ${row.id} 已${row.enabled ? '停用' : '启用'}`)
    } catch (reason) {
      setError(apiErrorMessage(reason))
    } finally {
      setPending(null)
    }
  }
  const columns: DataGridColumn<(typeof rows)[number]>[] = [
    {
      id: 'id',
      header: '规则 ID',
      isRowHeader: true,
      cell: (row) => String(row.id),
    },
    {
      id: 'name',
      header: '规则名称',
      cell: (row) => <TruncatedText text={row.name} />,
    },
    { id: 'category', header: '分类', cell: (row) => row.category },
    {
      id: 'level',
      header: '防护级别',
      cell: (row) => `PL${row.paranoia_level}`,
    },
    {
      id: 'enabled',
      header: '规则开关',
      cell: (row) => (
        <StatusChip
          label={row.enabled ? '已启用' : '已停用'}
          tone={row.enabled ? 'success' : 'default'}
          reserve={['已启用', '已停用']}
        />
      ),
    },
    {
      id: 'effective',
      header: '生效状态',
      cell: (row) => (
        <StatusChip
          label={
            !row.enabled
              ? '未生效（已停用）'
              : row.paranoia_level > policy.managed.paranoiaLevel
                ? `未生效（需 PL${row.paranoia_level}）`
                : '已生效'
          }
          tone={
            !row.enabled
              ? 'default'
              : row.paranoia_level > policy.managed.paranoiaLevel
                ? 'warning'
                : 'success'
          }
          reserve={[
            '已生效',
            '未生效（已停用）',
            ...[1, 2, 3, 4].map((level) => `未生效（需 PL${level}）`),
          ]}
        />
      ),
    },
    {
      id: 'action',
      header: '操作',
      cell: (row) => (
        <Button
          size='sm'
          variant='tertiary'
          isPending={pending === row.id}
          isDisabled={pending !== null || locked}
          aria-label={`${row.enabled ? '停用' : '启用'}规则 ${row.id}`}
          onPress={() => {
            void toggle(row)
          }}
        >
          {row.enabled ? '停用' : '启用'}
        </Button>
      ),
    },
  ]
  return (
    <>
      <ListToolbar
        search={{
          value: search.q,
          onChange: (q) => patch({ q, page: 1 }),
          placeholder: '搜索规则 ID、名称、分类或 PL 级别',
        }}
      />
      {error && (
        <p role='alert' className='mb-3 text-sm text-danger'>
          {error}
        </p>
      )}
      <DataTable
        label='托管 WAF 规则'
        columns={columns}
        data={rows}
        isLoading={false}
        variant='secondary'
        autoHeight
        empty={{ title: '暂无匹配的规则' }}
      />
    </>
  )
}
function PolicyConfiguration({
  policy,
  creating,
  children,
}: {
  policy: SecurityPolicy
  creating: boolean
  children?: (
    managedConfiguration: ReactNode,
    onManagedSaved: (revision: number) => void,
    configurationDirty: boolean
  ) => ReactNode
}) {
  const data = useSecurityData(),
    { router } = useSecuritySearch(),
    [value, setValue] = useState(() => ({
      name: policy.name,
      description: policy.description,
      managed: policy.managed,
    })),
    [error, setError] = useState(''),
    [saving, setSaving] = useState(false),
    [revision, setRevision] = useState(policy.revision)
  const clean = {
    name: policy.name,
    description: policy.description,
    managed: policy.managed,
  }
  const save = async () => {
    if (saving) return
    if (
      !Number.isInteger(value.managed.inboundThreshold) ||
      value.managed.inboundThreshold < 1 ||
      value.managed.inboundThreshold > 1000
    ) {
      setError('异常评分阈值应为 1–1000 的整数')
      return
    }
    const name = value.name.trim()
    if (!name || name.length > 80) {
      setError('请填写 1–80 个字符的规则集名称')
      return
    }
    if (
      data.policies.some((item) => item.id !== policy.id && item.name === name)
    ) {
      setError('规则集名称已存在')
      return
    }
    setSaving(true)
    try {
      const saved = await data.setPolicies(
        (policies) =>
          creating
            ? [...policies, { ...policy, ...value, name }]
            : policies.map((item) =>
                item.id === policy.id ? { ...item, ...value, name } : item
              ),
        revision
      )
      setRevision(saved.revision)
      setValue({ ...value, name })
      setError('')
      toast.success('规则集已保存')
      if (creating)
        void router.navigate({
          href: `/security/policies/${saved.id}`,
          replace: true,
          ignoreBlocker: true,
        })
    } catch (reason) {
      setError(apiErrorMessage(reason))
    } finally {
      setSaving(false)
    }
  }
  return (
    <>
      <Section title='基本配置'>
        <div className='grid gap-4 sm:grid-cols-2'>
          <Field
            label='规则集名称'
            value={value.name}
            maxLength={80}
            required
            onChange={(name) => setValue({ ...value, name })}
          />
          <Field
            label='描述'
            value={value.description}
            maxLength={200}
            onChange={(description) => setValue({ ...value, description })}
          />
        </div>
      </Section>
      {children?.(
        <div className='mb-4 grid gap-4 sm:grid-cols-2'>
          <Choice
            label='防护级别'
            value={String(value.managed.paranoiaLevel)}
            items={[1, 2, 3, 4].map((level) => ({
              id: String(level),
              label: `PL${level}`,
            }))}
            onChange={(level) =>
              setValue({
                ...value,
                managed: { ...value.managed, paranoiaLevel: Number(level) },
              })
            }
          />
          <Field
            label='异常评分阈值'
            type='number'
            min={1}
            max={1000}
            value={String(value.managed.inboundThreshold)}
            onChange={(threshold) =>
              setValue({
                ...value,
                managed: {
                  ...value.managed,
                  inboundThreshold: Number(threshold),
                },
              })
            }
          />
        </div>,
        setRevision,
        JSON.stringify(value) !== JSON.stringify(clean)
      )}
      <SaveBar
        isDirty={creating || JSON.stringify(value) !== JSON.stringify(clean)}
        isSaving={saving}
        onSave={save}
        onDiscard={() => {
          setValue(clean)
          setError('')
        }}
        error={error}
        saveLabel={creating ? '创建规则集' : '保存'}
      />
    </>
  )
}
export function SecurityRuleDetail() {
  const data = useSecurityData(),
    { search, router } = useSecuritySearch(),
    { policyId, ruleId } = useParams({ strict: false }) as {
      policyId: string
      ruleId: string
    }
  const policy = data.policies.find((item) => item.id === policyId),
    creating = ruleId === 'new',
    [starter] = useState(() =>
      newRule(search.stage === 'rate' ? 'rate' : 'custom')
    ),
    [tab, setTab] = useState<'config' | 'test'>('config'),
    [revision, setRevision] = useState(policy?.revision)
  const rule = creating
    ? starter
    : policy?.rules.find(
        (item) => item.id === ruleId && item.stage !== 'managed'
      )
  const back = () => {
    void router.navigate({ href: `/security/policies/${policyId}` })
  }
  if (!rule || !policy)
    return (
      <Page
        title='规则不存在'
        actions={
          <Button size='sm' onPress={back}>
            返回规则集
          </Button>
        }
      >
        {null}
      </Page>
    )
  return (
    <Page
      title={
        creating
          ? rule.stage === 'rate'
            ? '创建 CC / 限速规则'
            : '创建自定义规则'
          : rule.name
      }
      description={policy.name}
      actions={
        <Button size='sm' variant='tertiary' onPress={back}>
          <ArrowLeft />
          返回
        </Button>
      }
    >
      <PageTabs
        label='规则详情'
        selected={tab}
        onSelectionChange={setTab}
        items={
          creating
            ? [{ id: 'config', label: '配置' }]
            : [
                { id: 'config', label: '配置' },
                { id: 'test', label: '测试' },
              ]
        }
      >
        {tab === 'config' ? (
          <SecurityRuleEditor
            key={rule.id}
            rule={rule}
            creating={creating}
            rules={policy.rules}
            lists={data.lists}
            onSave={async (saved, position) => {
              const updated = await data.setPolicies(
                (policies) =>
                  policies.map((item) => {
                    if (item.id !== policy.id) return item
                    const next = item.rules.filter(
                        (entry) => entry.id !== saved.id
                      ),
                      peers = next.filter(
                        (entry) => entry.stage === saved.stage
                      )
                    let index =
                      position === 'keep' && !creating
                        ? item.rules.findIndex((entry) => entry.id === saved.id)
                        : position === 'first'
                          ? peers.length
                            ? next.indexOf(peers[0])
                            : next.length
                          : position === 'last'
                            ? peers.length
                              ? next.indexOf(peers[peers.length - 1]) + 1
                              : next.length
                            : next.findIndex((entry) => entry.id === position) +
                              1
                    if (index < 0) index = next.length
                    next.splice(index, 0, saved)
                    return { ...item, rules: next }
                  }),
                revision
              )
              setRevision(updated.revision)
              toast.success('规则已保存')
              if (creating)
                void router.navigate({
                  href: `/security/policies/${policy.id}/rules/${saved.id}`,
                  replace: true,
                  ignoreBlocker: true,
                })
              const persisted = updated.rules.find(
                (item) => item.id === saved.id
              )
              if (!persisted) throw new Error('保存后的规则不存在')
              return persisted
            }}
          />
        ) : (
          <RuleTest policy={policy} rule={rule} />
        )}
      </PageTabs>
    </Page>
  )
}
function RuleTest({ policy, rule }: { policy: SecurityPolicy; rule: Rule }) {
  const data = useSecurityData(),
    { router } = useSecuritySearch(),
    sites = data.websites.filter((website) => website.policyId === policy.id)
  const defaults: RequestSample = {
    ip: '203.0.113.66',
    path: '/api/login',
    method: 'POST',
    host: sites[0]?.name ?? 'example.com',
    user_agent: 'curl/8.0',
    country: 'CN',
    asn: '64500',
  }
  for (const condition of rule.conditions)
    if (['eq', 'starts_with', 'contains'].includes(condition.operator))
      defaults[condition.field] = condition.value
  const [request, setRequest] = useState(defaults),
    [count, setCount] = useState(String(rule.limit + 1)),
    [websiteId, setWebsiteId] = useState(sites[0]?.id ?? ''),
    [error, setError] = useState(''),
    [result, setResult] = useState<{ matched: boolean; message: string }>(),
    [testing, setTesting] = useState(false)
  return (
    <Section title='测试请求'>
      <form
        className='flex max-w-4xl flex-col gap-4'
        noValidate
        onSubmit={async (event) => {
          event.preventDefault()
          if (testing) return
          setTesting(true)
          try {
            setResult(
              await data.testRule(
                policy.id,
                rule.id,
                request,
                Number(count),
                websiteId
              )
            )
            setError('')
          } catch (reason) {
            setError(apiErrorMessage(reason))
          } finally {
            setTesting(false)
          }
        }}
      >
        <Choice
          label='网站'
          value={websiteId}
          items={sites.map((website) => ({
            id: website.id,
            label: website.name,
          }))}
          onChange={(id) => {
            setWebsiteId(id)
            setRequest({
              ...request,
              host:
                sites.find((website) => website.id === id)?.name ??
                request.host,
            })
          }}
          hint={sites.length ? undefined : '请先应用规则集到网站'}
        />
        <div className='grid gap-4 sm:grid-cols-2'>
          {(
            [
              { id: 'ip', label: '客户端 IP' },
              { id: 'asn', label: 'ASN' },
              { id: 'country', label: '国家代码' },
              { id: 'method', label: '请求方法' },
              { id: 'path', label: 'URI 路径' },
              { id: 'user_agent', label: 'User-Agent' },
            ] as const
          ).map((field) => (
            <Field
              key={field.id}
              label={field.label}
              value={request[field.id]}
              onChange={(value) =>
                setRequest({ ...request, [field.id]: value })
              }
            />
          ))}
        </div>
        {rule.stage === 'rate' && (
          <Field
            label={`窗口内请求数（${rule.window} 秒）`}
            type='number'
            min={1}
            value={count}
            onChange={setCount}
          />
        )}
        {error && (
          <p role='alert' className='text-sm text-danger'>
            {error}
          </p>
        )}
        <div className='flex flex-wrap gap-2'>
          <Button
            size='sm'
            type='submit'
            isDisabled={!sites.length}
            isPending={testing}
          >
            模拟触发
          </Button>
        </div>
        {result && (
          <div className='flex flex-col gap-3 rounded-xl border border-border p-4'>
            <StatusChip
              label={result.matched ? '已触发' : '未触发'}
              tone={result.matched ? 'success' : 'default'}
            />
            <p className='text-sm'>{result.message}</p>
            {result.matched && rule.action === 'block' && (
              <pre className='text-xs whitespace-pre-wrap'>
                {rule.response.enabled
                  ? `HTTP ${rule.response.status}\nContent-Type: ${rule.response.content_type}\n\n${rule.response.body}`
                  : `HTTP ${rule.stage === 'rate' ? 429 : 403}`}
              </pre>
            )}
            {result.matched && rule.enrollment.enabled && (
              <Button
                size='sm'
                variant='secondary'
                onPress={() => {
                  void router.navigate({
                    href: `/security/lists/${rule.enrollment.list_id}`,
                  })
                }}
              >
                查看目标清单
              </Button>
            )}
          </div>
        )}
      </form>
    </Section>
  )
}
