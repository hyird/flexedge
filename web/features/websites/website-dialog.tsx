import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { Button, Tabs, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import { Confirm } from '@/components/confirm'
import { Drawer } from '@/components/drawer'
import { Choice, Field, FormActions, Notice, Toggle } from '@/components/forms'
import { QueryNotice } from '@/components/page'
import { cachePolicyOptionsQuery } from '@/features/cache-policies/data'
import { usableCertificateOptionsQuery } from '@/features/certificates/data'
import { clusterOptionsQuery } from '@/features/clusters/data'
import { ArrayLines } from './array-lines'
import { saveWebsite } from './data'
import {
  collectOriginGroups,
  originRemovalError,
  renameOriginGroup,
} from './origin-groups'
import type { Website } from './types'
import {
  setWebsiteHttpsEnabled,
  websiteCertificateOptions,
  websiteFormSchema,
  type WebsiteFormValues,
} from './website-form'
import {
  websiteFormToConfig,
  websiteToFormValues,
} from './website-form-mapping'
import { WebsiteRouteEditor } from './website-route-editor'

const sections = [
  ['basic', '基本'],
  ['domains', '域名'],
  ['https', 'TLS'],
  ['origins', '源站'],
  ['origin-settings', '回源'],
  ['health', '健康检查'],
  ['routes', '路由'],
  ['cache', '缓存'],
  ['compression', '压缩'],
  ['logs', '日志'],
] as const

export function WebsiteDialog({
  website,
  onClose,
}: {
  website?: Website
  onClose: () => void
}) {
  const client = useQueryClient()
  const [initial] = useState(() => websiteToFormValues(website))
  const [values, setValues] = useState(initial)
  const [section, setSection] = useState('basic')
  const [errors, setErrors] = useState<Record<string, string>>({})
  const [leaving, setLeaving] = useState(false)
  const [failure, setFailure] = useState('')
  const [renamingGroup, setRenamingGroup] = useState<string | null>(null)
  const [groupDraft, setGroupDraft] = useState('')
  const [groupError, setGroupError] = useState('')
  const clusters = useQuery({
    ...clusterOptionsQuery,
    enabled: section === 'basic',
  })
  const certificates = useQuery({
    ...usableCertificateOptionsQuery,
    enabled: section === 'https',
  })
  const policies = useQuery({
    ...cachePolicyOptionsQuery,
    enabled: section === 'cache',
  })
  const mutation = useMutation({
    mutationFn: (input: WebsiteFormValues) =>
      saveWebsite(
        {
          cluster_id: input.cluster_id,
          status: input.status,
          config: websiteFormToConfig(input),
        },
        website
      ),
    onSuccess: () => {
      toast.success('网站配置已保存并分发')
      void client.invalidateQueries({ queryKey: queryKeys.websites })
      onClose()
    },
    onError: (error) => setFailure(apiErrorMessage(error)),
  })
  function set<K extends keyof WebsiteFormValues>(
    key: K,
    value: WebsiteFormValues[K]
  ) {
    setValues((current) => ({ ...current, [key]: value }))
  }
  const originGroups = collectOriginGroups(values.origins)
  const groups = originGroups.map((group) => group.name)
  const certificateOptions = websiteCertificateOptions(
    certificates.data ?? [],
    values.certificate_ids,
    website?.certificates ?? []
  )
  function requestClose() {
    if (mutation.isPending) return
    if (JSON.stringify(initial) !== JSON.stringify(values)) setLeaving(true)
    else onClose()
  }
  function submit(event: React.FormEvent) {
    event.preventDefault()
    const parsed = websiteFormSchema.safeParse(values)
    if (!parsed.success) {
      const issues: Record<string, string> = {}
      for (const issue of parsed.error.issues)
        issues[issue.path.join('.')] ??= issue.message
      setErrors(issues)
      const field = String(parsed.error.issues[0]?.path[0] ?? '')
      setSection(
        field === 'domains' || field === 'origins'
          ? field
          : field === 'route_rules'
            ? 'routes'
            : field.startsWith('cache_')
              ? 'cache'
              : field.startsWith('response_compression_')
                ? 'compression'
                : field.startsWith('health') ||
                    ['healthy_threshold', 'unhealthy_threshold'].includes(field)
                  ? 'health'
                  : field.startsWith('access_log_')
                    ? 'logs'
                    : [
                          'https_enabled',
                          'certificate_ids',
                          'minimum_tls_version',
                          'force_https',
                          'http2_enabled',
                          'hsts_enabled',
                        ].includes(field)
                      ? 'https'
                      : [
                            'origin_connect_timeout_seconds',
                            'origin_read_timeout_seconds',
                            'pass_client_ip',
                          ].includes(field)
                        ? 'origin-settings'
                        : 'basic'
      )
      setFailure('请检查标出的配置项。')
      return
    }
    setErrors({})
    setFailure('')
    mutation.mutate(parsed.data)
  }
  function field(key: keyof WebsiteFormValues, label: string, hint?: string) {
    const numeric = typeof values[key] === 'number'
    return (
      <Field
        key={key}
        label={label}
        value={String(values[key])}
        type={numeric ? 'number' : 'text'}
        onChange={(value) =>
          set(
            key,
            numeric ? (value === '' ? Number.NaN : Number(value)) : value
          )
        }
        hint={hint}
        error={errors[key]}
        disabled={mutation.isPending}
      />
    )
  }
  function toggle(key: keyof WebsiteFormValues, label: string, hint?: string) {
    return (
      <Toggle
        key={key}
        label={label}
        hint={hint}
        selected={Boolean(values[key])}
        onChange={(value) => set(key, value)}
        disabled={mutation.isPending}
      />
    )
  }
  return (
    <>
      <Drawer
        title={
          website
            ? `编辑 ${website.config.name || website.access_domain}`
            : '创建网站'
        }
        onClose={requestClose}
        busy={mutation.isPending}
        size='lg'
      >
        <form onSubmit={submit} className='flex flex-col gap-5'>
          <p className='text-sm text-muted'>
            配置域名、源站及分发策略，保存后发布到所属集群。
          </p>
          <Tabs
            selectedKey={section}
            onSelectionChange={(key) => setSection(String(key))}
            variant='secondary'
          >
            <Tabs.ListContainer>
              <Tabs.List aria-label='网站配置分组'>
                {sections.map(([id, label]) => (
                  <Tabs.Tab key={id} id={id}>
                    {label}
                    <Tabs.Indicator />
                  </Tabs.Tab>
                ))}
              </Tabs.List>
            </Tabs.ListContainer>
            <Tabs.Panel id={section} className='pt-4'>
              <Notice>{failure}</Notice>
              <div className='min-h-64 space-y-5' inert={mutation.isPending}>
                {section === 'basic' && (
                  <>
                    <QueryNotice
                      query={clusters}
                      onRetry={() => void clusters.refetch()}
                    />
                    <div className='grid gap-4 sm:grid-cols-2'>
                      {field('name', '网站名称')}
                      <Choice
                        label='所属集群'
                        value={values.cluster_id}
                        onChange={(value) => set('cluster_id', value)}
                        items={(clusters.data ?? []).map((cluster) => ({
                          id: cluster.id,
                          label: cluster.name,
                        }))}
                        disabled={mutation.isPending}
                      />
                      <Choice
                        label='状态'
                        value={values.status}
                        onChange={(value) =>
                          set('status', value as WebsiteFormValues['status'])
                        }
                        items={[
                          { id: 'enabled', label: '启用' },
                          { id: 'disabled', label: '停用' },
                        ]}
                      />
                      <Choice
                        label='默认源站组'
                        value={values.default_origin_group}
                        onChange={(value) => set('default_origin_group', value)}
                        items={groups.map((group) => ({
                          id: group,
                          label: group,
                        }))}
                      />
                      {field(
                        'origin_host_header',
                        '回源 Host',
                        '$host 保留客户端请求域名，也可指定源站域名。'
                      )}
                    </div>
                    {(errors.cluster_id || errors.default_origin_group) && (
                      <Notice>
                        {errors.cluster_id || errors.default_origin_group}
                      </Notice>
                    )}
                  </>
                )}
                {section === 'domains' && (
                  <>
                    <p className='text-sm text-muted'>
                      托管模式自动维护解析；外部 DNS 请把域名 CNAME
                      到集群接入域名。
                    </p>
                    {values.domains.map((domain, index) => (
                      <div
                        key={domain.id}
                        className='grid items-end gap-3 sm:grid-cols-[1fr_10rem_auto]'
                      >
                        <Field
                          label={`域名 ${index + 1}`}
                          value={domain.hostname}
                          onChange={(hostname) =>
                            set(
                              'domains',
                              values.domains.map((item) =>
                                item.id === domain.id
                                  ? { ...item, hostname }
                                  : item
                              )
                            )
                          }
                          error={errors[`domains.${index}.hostname`]}
                          placeholder='www.example.com'
                        />
                        <Choice
                          label='DNS 模式'
                          value={domain.dns_mode}
                          items={[
                            { id: 'managed', label: '托管 DNS' },
                            { id: 'external', label: '外部 DNS' },
                          ]}
                          onChange={(mode) =>
                            set(
                              'domains',
                              values.domains.map((item) =>
                                item.id === domain.id
                                  ? {
                                      ...item,
                                      dns_mode: mode as typeof domain.dns_mode,
                                    }
                                  : item
                              )
                            )
                          }
                        />
                        <Button
                          variant='danger-soft'
                          size='sm'
                          onPress={() =>
                            set(
                              'domains',
                              values.domains.filter(
                                (item) => item.id !== domain.id
                              )
                            )
                          }
                        >
                          移除
                        </Button>
                      </div>
                    ))}
                    <Button
                      size='sm'
                      variant='secondary'
                      onPress={() =>
                        set('domains', [
                          ...values.domains,
                          {
                            id: crypto.randomUUID(),
                            hostname: '',
                            dns_mode: 'managed',
                          },
                        ])
                      }
                      isDisabled={values.domains.length >= 100}
                    >
                      添加域名
                    </Button>
                    <Notice>{errors.domains}</Notice>
                  </>
                )}
                {section === 'https' && (
                  <>
                    <Toggle
                      label='启用 HTTPS'
                      selected={values.https_enabled}
                      onChange={(enabled) =>
                        setValues((current) =>
                          setWebsiteHttpsEnabled(current, enabled)
                        )
                      }
                      disabled={mutation.isPending}
                    />
                    {values.https_enabled && (
                      <>
                        <QueryNotice
                          query={certificates}
                          onRetry={() => void certificates.refetch()}
                        />
                        <div className='space-y-1'>
                          <h3 className='text-sm font-medium'>绑定证书</h3>
                          <p className='text-xs text-muted'>
                            可选择多张可用证书覆盖网站域名。
                          </p>
                          {certificateOptions.map((certificate) => (
                            <Toggle
                              key={certificate.id}
                              label={
                                certificate.domains.join('、') || certificate.id
                              }
                              selected={values.certificate_ids.includes(
                                certificate.id
                              )}
                              hint={
                                certificate.usable
                                  ? undefined
                                  : certificates.isPending ||
                                      certificates.isError
                                    ? '正在读取可用证书列表，当前绑定仍可移除。'
                                    : '此证书不可用，请取消绑定并选择可用证书。'
                              }
                              onChange={(selected) =>
                                set(
                                  'certificate_ids',
                                  selected
                                    ? [
                                        ...values.certificate_ids,
                                        certificate.id,
                                      ]
                                    : values.certificate_ids.filter(
                                        (id) => id !== certificate.id
                                      )
                                )
                              }
                            />
                          ))}
                          {!certificates.isPending &&
                            !certificates.data?.length && (
                              <p className='py-3 text-sm text-muted'>
                                暂无可用证书，请先在证书页面签发。
                              </p>
                            )}
                        </div>
                        <Notice>{errors.certificate_ids}</Notice>
                        <Choice
                          label='最低 TLS 版本'
                          value={values.minimum_tls_version}
                          onChange={(value) =>
                            set('minimum_tls_version', value as '1.2' | '1.3')
                          }
                          items={[
                            { id: '1.2', label: 'TLS 1.2' },
                            { id: '1.3', label: 'TLS 1.3' },
                          ]}
                        />
                        {toggle('force_https', 'HTTP 自动跳转 HTTPS')}
                        {toggle('http2_enabled', '启用 HTTP/2')}
                        {toggle(
                          'hsts_enabled',
                          '启用 HSTS',
                          '通知浏览器后续仅通过 HTTPS 访问。'
                        )}
                      </>
                    )}
                  </>
                )}
                {section === 'origins' && (
                  <>
                    <p className='text-sm text-muted'>
                      相同源站组中的主源按权重分发，主源不可用时使用备用源站。
                    </p>
                    <div className='space-y-2'>
                      <h3 className='text-sm font-medium'>源站组</h3>
                      {originGroups.map((group) => (
                        <div
                          key={group.name}
                          className='flex flex-wrap items-center justify-between gap-2 text-sm'
                        >
                          <span>
                            {group.name}{' '}
                            <span className='ml-2 text-xs text-muted tabular-nums'>
                              {group.enabledPrimaryCount} 主源 /{' '}
                              {group.enabledCount} 启用
                            </span>
                          </span>
                          <Button
                            size='sm'
                            variant='tertiary'
                            onPress={() => {
                              setRenamingGroup(group.name)
                              setGroupDraft(group.name)
                              setGroupError('')
                            }}
                          >
                            重命名
                          </Button>
                        </div>
                      ))}
                      {renamingGroup && (
                        <div className='flex flex-wrap items-end gap-2'>
                          <div className='min-w-48 flex-1'>
                            <Field
                              label='源站组新名称'
                              value={groupDraft}
                              onChange={setGroupDraft}
                            />
                          </div>
                          <Button
                            size='sm'
                            variant='secondary'
                            onPress={() => {
                              const result = renameOriginGroup(
                                values,
                                renamingGroup,
                                groupDraft
                              )
                              if (!result.ok) {
                                setGroupError(result.message)
                                return
                              }
                              setValues((current) => ({
                                ...current,
                                ...result.value,
                              }))
                              setRenamingGroup(null)
                              setGroupError('')
                            }}
                          >
                            应用
                          </Button>
                          <Button
                            size='sm'
                            variant='tertiary'
                            onPress={() => {
                              setRenamingGroup(null)
                              setGroupError('')
                            }}
                          >
                            取消
                          </Button>
                        </div>
                      )}
                      <Notice>{groupError}</Notice>
                    </div>
                    {values.origins.map((origin, index) => {
                      const update = (patch: Partial<typeof origin>) =>
                        set(
                          'origins',
                          values.origins.map((item) =>
                            item.id === origin.id ? { ...item, ...patch } : item
                          )
                        )
                      return (
                        <section
                          key={origin.id}
                          className='space-y-3 rounded-2xl bg-surface-secondary p-4'
                        >
                          <div className='flex items-center justify-between'>
                            <h3 className='text-sm font-medium'>
                              源站 {index + 1}
                            </h3>
                            <Button
                              variant='danger-soft'
                              size='sm'
                              onPress={() => {
                                const problem = originRemovalError(
                                  values,
                                  index
                                )
                                if (problem) {
                                  setGroupError(problem)
                                  return
                                }
                                set(
                                  'origins',
                                  values.origins.filter(
                                    (item) => item.id !== origin.id
                                  )
                                )
                                setGroupError('')
                              }}
                            >
                              移除
                            </Button>
                          </div>
                          <div className='grid gap-3 sm:grid-cols-2 lg:grid-cols-3'>
                            <Field
                              label='源站组'
                              value={origin.group}
                              onChange={(group) => update({ group })}
                              error={errors[`origins.${index}.group`]}
                            />
                            <Field
                              label='主机 / IP'
                              value={origin.host}
                              onChange={(host) => update({ host })}
                              error={errors[`origins.${index}.host`]}
                            />
                            <Choice
                              label='协议'
                              value={origin.protocol}
                              onChange={(protocol) =>
                                update({
                                  protocol: protocol as 'http' | 'https',
                                })
                              }
                              items={[
                                { id: 'http', label: 'HTTP' },
                                { id: 'https', label: 'HTTPS' },
                              ]}
                            />
                            <Field
                              label='端口'
                              type='number'
                              value={String(origin.port)}
                              onChange={(port) =>
                                update({ port: Number(port) })
                              }
                              error={errors[`origins.${index}.port`]}
                            />
                            <Choice
                              label='角色'
                              value={origin.role}
                              onChange={(role) =>
                                update({ role: role as 'primary' | 'backup' })
                              }
                              items={[
                                { id: 'primary', label: '主源站' },
                                { id: 'backup', label: '备用源站' },
                              ]}
                            />
                            <Field
                              label='权重'
                              type='number'
                              value={String(origin.weight)}
                              onChange={(weight) =>
                                update({ weight: Number(weight) })
                              }
                              error={errors[`origins.${index}.weight`]}
                            />
                          </div>
                          <Toggle
                            label='启用源站'
                            selected={origin.status === 'enabled'}
                            onChange={(enabled) =>
                              update({
                                status: enabled ? 'enabled' : 'disabled',
                              })
                            }
                          />
                        </section>
                      )
                    })}
                    <Button
                      variant='secondary'
                      size='sm'
                      onPress={() =>
                        set('origins', [
                          ...values.origins,
                          {
                            id: crypto.randomUUID(),
                            group: values.default_origin_group || 'default',
                            protocol: 'http',
                            host: '',
                            port: 80,
                            role: 'primary',
                            weight: 100,
                            status: 'enabled',
                          },
                        ])
                      }
                      isDisabled={values.origins.length >= 100}
                    >
                      添加源站
                    </Button>
                    <Notice>{errors.origins}</Notice>
                  </>
                )}
                {section === 'origin-settings' && (
                  <>
                    <div className='grid gap-4 sm:grid-cols-2'>
                      {field(
                        'origin_connect_timeout_seconds',
                        '连接超时（秒）'
                      )}
                      {field('origin_read_timeout_seconds', '读取超时（秒）')}
                    </div>
                    {toggle(
                      'pass_client_ip',
                      '传递客户端 IP',
                      '通过转发头传递客户端地址。'
                    )}
                  </>
                )}
                {section === 'health' && (
                  <>
                    {toggle('health_check_enabled', '主动健康检查')}
                    {values.health_check_enabled && (
                      <div className='grid gap-4 sm:grid-cols-2'>
                        {field('health_check_path', '检查路径')}
                        {field('health_check_expected_status', '预期状态码')}
                        {field(
                          'health_check_interval_seconds',
                          '检查间隔（秒）'
                        )}
                        {field(
                          'health_check_timeout_seconds',
                          '检查超时（秒）'
                        )}
                        {field('healthy_threshold', '恢复健康阈值')}
                        {field('unhealthy_threshold', '判定失败阈值')}
                      </div>
                    )}
                  </>
                )}
                {section === 'routes' && (
                  <WebsiteRouteEditor
                    rules={values.route_rules}
                    onChange={(rules) => set('route_rules', rules)}
                    groups={groups}
                    errors={errors}
                  />
                )}
                {section === 'cache' && (
                  <>
                    {toggle('cache_enabled', '启用边缘缓存')}
                    {values.cache_enabled && (
                      <>
                        <QueryNotice
                          query={policies}
                          onRetry={() => void policies.refetch()}
                        />
                        <Choice
                          label='缓存策略'
                          value={values.cache_policy_id || 'none'}
                          onChange={(value) =>
                            set(
                              'cache_policy_id',
                              value === 'none' ? '' : value
                            )
                          }
                          items={[
                            { id: 'none', label: '请选择缓存策略' },
                            ...(policies.data ?? []).map((policy) => ({
                              id: policy.id,
                              label: `${policy.name}${policy.status === 'disabled' ? '（已停用）' : ''}`,
                            })),
                          ]}
                        />
                        <Notice>{errors.cache_policy_id}</Notice>
                        <p className='text-sm text-muted'>
                          缓存规则统一在缓存策略页面维护。缓存刷新、预热和任务结果可从网站列表的操作菜单进入。
                        </p>
                      </>
                    )}
                  </>
                )}
                {section === 'compression' && (
                  <>
                    {toggle('response_compression_enabled', '启用响应压缩')}
                    {values.response_compression_enabled && (
                      <>
                        <div className='grid gap-4 sm:grid-cols-2'>
                          {field(
                            'response_compression_min_bytes',
                            '最小响应大小（字节）'
                          )}
                          {field(
                            'response_compression_max_bytes',
                            '最大响应大小（字节）',
                            '0 表示不限制。'
                          )}
                        </div>
                        <div className='space-y-3'>
                          <h3 className='text-sm font-medium'>
                            压缩算法优先级
                          </h3>
                          <p className='text-xs text-muted'>
                            按列表顺序选择客户端支持的算法。
                          </p>
                          {values.response_compression_algorithms.map(
                            (algorithm, index) => (
                              <div
                                key={algorithm}
                                className='flex items-center gap-2'
                              >
                                <span className='min-w-24 flex-1 text-sm tabular-nums'>
                                  {index + 1}. {algorithm}
                                </span>
                                {([-1, 1] as const).map((direction) => (
                                  <Button
                                    key={direction}
                                    size='sm'
                                    variant='tertiary'
                                    isDisabled={
                                      index + direction < 0 ||
                                      index + direction >=
                                        values.response_compression_algorithms
                                          .length
                                    }
                                    onPress={() => {
                                      const next = [
                                        ...values.response_compression_algorithms,
                                      ]
                                      ;[next[index], next[index + direction]] =
                                        [next[index + direction], next[index]]
                                      set(
                                        'response_compression_algorithms',
                                        next
                                      )
                                    }}
                                  >
                                    {direction === -1 ? '上移' : '下移'}
                                  </Button>
                                ))}
                                <Button
                                  size='sm'
                                  variant='danger-soft'
                                  onPress={() =>
                                    set(
                                      'response_compression_algorithms',
                                      values.response_compression_algorithms.filter(
                                        (item) => item !== algorithm
                                      )
                                    )
                                  }
                                >
                                  移除
                                </Button>
                              </div>
                            )
                          )}
                          {values.response_compression_algorithms.length <
                            3 && (
                            <Choice
                              compact
                              label='添加压缩算法'
                              value='add'
                              onChange={(value) => {
                                if (value !== 'add')
                                  set('response_compression_algorithms', [
                                    ...values.response_compression_algorithms,
                                    value as 'br' | 'zstd' | 'gzip',
                                  ])
                              }}
                              items={[
                                { id: 'add', label: '添加算法' },
                                ...(['br', 'zstd', 'gzip'] as const)
                                  .filter(
                                    (algorithm) =>
                                      !values.response_compression_algorithms.includes(
                                        algorithm
                                      )
                                  )
                                  .map((algorithm) => ({
                                    id: algorithm,
                                    label: algorithm,
                                  })),
                              ]}
                            />
                          )}
                          <Notice>
                            {errors.response_compression_algorithms}
                          </Notice>
                        </div>
                        <div className='grid gap-4 sm:grid-cols-2'>
                          <ArrayLines
                            label='MIME 类型'
                            value={values.response_compression_mime_types}
                            onChange={(items) =>
                              set('response_compression_mime_types', items)
                            }
                            hint='每行一项，可使用 text/*。'
                          />
                          <ArrayLines
                            label='扩展名'
                            value={values.response_compression_extensions}
                            onChange={(items) =>
                              set('response_compression_extensions', items)
                            }
                            hint='每行一项，如 .js。'
                          />
                          <ArrayLines
                            label='排除扩展名'
                            value={
                              values.response_compression_excluded_extensions
                            }
                            onChange={(items) =>
                              set(
                                'response_compression_excluded_extensions',
                                items
                              )
                            }
                          />
                        </div>
                      </>
                    )}
                  </>
                )}
                {section === 'logs' && (
                  <>
                    {toggle('access_log_enabled', '记录访问日志')}
                    {values.access_log_enabled && (
                      <>
                        <div className='grid gap-x-6 sm:grid-cols-2'>
                          {toggle('access_log_request_headers', '请求头')}
                          {toggle('access_log_response_headers', '响应头')}
                          {toggle('access_log_request_body', '请求体')}
                          {toggle('access_log_query_params', '查询参数')}
                          {toggle('access_log_cookies', 'Cookie')}
                          {toggle('access_log_referer', 'Referer')}
                          {toggle('access_log_user_agent', 'User-Agent')}
                          {toggle('access_log_client_abort', '客户端中断')}
                        </div>
                        <h3 className='text-sm font-medium'>记录的状态码</h3>
                        <div className='grid gap-x-6 sm:grid-cols-3'>
                          {(['1xx', '2xx', '3xx', '4xx', '5xx'] as const).map(
                            (range) => (
                              <Toggle
                                key={range}
                                label={range}
                                selected={values.access_log_status_code_ranges.includes(
                                  range
                                )}
                                onChange={(selected) =>
                                  set(
                                    'access_log_status_code_ranges',
                                    selected
                                      ? [
                                          ...values.access_log_status_code_ranges,
                                          range,
                                        ]
                                      : values.access_log_status_code_ranges.filter(
                                          (item) => item !== range
                                        )
                                  )
                                }
                              />
                            )
                          )}
                        </div>
                      </>
                    )}
                  </>
                )}
              </div>
            </Tabs.Panel>
          </Tabs>
          <FormActions
            onCancel={requestClose}
            busy={mutation.isPending}
            label='保存并分发'
          />
        </form>
      </Drawer>
      {leaving && (
        <Confirm
          title='放弃未保存的配置？'
          description='当前更改尚未保存，关闭后将丢失这些更改。'
          onClose={() => setLeaving(false)}
          onConfirm={onClose}
        />
      )}
    </>
  )
}
