import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import {
  ArrowDown,
  ArrowRotateLeft,
  ArrowUp,
  Pencil,
  Plus,
  TrashBin,
} from '@gravity-ui/icons'
import { Button, Chip } from '@heroui/react'
import { formatDate } from '@/lib/format'
import { cn } from '@/lib/utils'
import { Choice, Field, Notice, Toggle } from '@/components/forms'
import { Mono } from '@/components/mono'
import { QueryNotice } from '@/components/page'
import { Section } from '@/components/section'
import { uncoveredHostnames } from '@/features/certificates/certificate-coverage'
import { usableCertificateOptionsQuery } from '@/features/certificates/data'
import { clusterOptionsQuery } from '@/features/clusters/data'
import type { DnsZoneOption } from '@/features/dns-zones/types'
import { ArrayLines } from './array-lines'
import { hostedZoneOptionsQuery } from './data'
import {
  composeHostedDomain,
  findHostedZone,
  splitHostedDomain,
} from './hosted-domain'
import {
  collectOriginGroups,
  nextOriginGroupName,
  originRemovalError,
  renameOriginGroup,
} from './origin-groups'
import {
  CheckOption,
  FieldNotice,
  IconAction,
  NewWindowLink,
  NumberField,
} from './settings-controls'
import type { Website, WebsiteOrigin } from './types'
import {
  setWebsiteHttpsEnabled,
  websiteCertificateOptions,
  websiteHostnames,
  type WebsiteFormValues,
} from './website-form'
import { WebsiteLifecycleActions } from './website-lifecycle-actions'
import type { WebsiteFormErrors } from './website-sections'

export type WebsiteSectionProps = {
  values: WebsiteFormValues
  errors: WebsiteFormErrors
  update: <K extends keyof WebsiteFormValues>(
    key: K,
    value: WebsiteFormValues[K]
  ) => void
  replace: (change: (current: WebsiteFormValues) => WebsiteFormValues) => void
  /** Saved website in edit mode; absent while creating. */
  website?: Website
}

const statusItems = [
  { id: 'enabled', label: '启用' },
  { id: 'disabled', label: '停用' },
]

function RefreshOptions({
  label,
  isPending,
  onRefresh,
}: {
  label: string
  isPending: boolean
  onRefresh: () => void
}) {
  return (
    <Button
      size='sm'
      variant='tertiary'
      isPending={isPending}
      onPress={onRefresh}
    >
      <ArrowRotateLeft className='size-3.5' />
      {label}
    </Button>
  )
}

export function BasicSection({
  values,
  errors,
  update,
  website,
}: WebsiteSectionProps) {
  const clusters = useQuery(clusterOptionsQuery)
  const cluster = clusters.data?.find((item) => item.id === values.cluster_id)
  return (
    <Section title='基本信息' description='网站名称、所属集群与启停状态。'>
      <div className='flex flex-col gap-4'>
        <QueryNotice
          query={clusters}
          title='集群列表加载失败'
          onRetry={() => void clusters.refetch()}
        />
        <div className='grid gap-4 sm:grid-cols-2'>
          <Field
            label='网站名称'
            value={values.name}
            onChange={(name) => update('name', name)}
            error={errors.name}
            maxLength={100}
          />
          <Choice
            label='状态'
            value={values.status}
            onChange={(status) =>
              update('status', status as WebsiteFormValues['status'])
            }
            items={statusItems}
            hint='停用后边缘节点不再为该网站提供服务。'
          />
          <div className='flex flex-col gap-1.5'>
            <Choice
              label='所属集群'
              value={values.cluster_id}
              onChange={(clusterId) => update('cluster_id', clusterId)}
              items={(clusters.data ?? []).map((item) => ({
                id: item.id,
                label: `${item.name}${item.status === 'disabled' ? '（已停用）' : ''}`,
              }))}
              error={errors.cluster_id}
              hint={
                cluster
                  ? `接入域名 ${cluster.access_domain} · 节点 ${cluster.online_node_count}/${cluster.node_count} 在线`
                  : '配置会分发到所选集群的全部边缘节点。'
              }
            />
            <div className='flex flex-wrap items-center gap-x-4 gap-y-1'>
              {cluster && (
                <NewWindowLink href={`/clusters/${cluster.id}`}>
                  查看集群
                </NewWindowLink>
              )}
              <NewWindowLink href='/clusters?create=1'>创建集群</NewWindowLink>
              <RefreshOptions
                label='刷新集群'
                isPending={clusters.isFetching}
                onRefresh={() => void clusters.refetch()}
              />
            </div>
          </div>
        </div>
        {website && <WebsiteLifecycleActions />}
      </div>
    </Section>
  )
}

type WebsiteDomain = WebsiteFormValues['domains'][number]

const dnsModeItems = [
  { id: 'managed', label: '托管 DNS' },
  { id: 'external', label: '外部 DNS' },
]

/** Why a zone is not ready yet; every zone stays selectable regardless. */
function hostedZoneState(zone: DnsZoneOption) {
  if (zone.available) return undefined
  if (zone.sync_status === 'failed' || zone.sync_status === 'conflict')
    return { label: '同步异常', danger: true }
  return {
    label: zone.sync_status === 'synced' ? '不可用' : '未同步',
    danger: false,
  }
}

function hostedZoneLabel(zone: DnsZoneOption) {
  const state = hostedZoneState(zone)
  return `${zone.domain} · ${zone.dns_provider_name}${state ? ` · ${state.label}` : ''}`
}

/** Zone + host prefix editor of a managed domain. The selection is local:
 * until the user changes it, it is split from the hostname by the owning
 * zone (nothing matching leaves the zone empty and the hostname in the
 * prefix); every change writes the composed hostname back to the form. */
function HostedDomainFields({
  domain,
  index,
  zones,
  zonesLoading,
  errors,
  onHostnameChange,
}: {
  domain: WebsiteDomain
  index: number
  zones: readonly DnsZoneOption[]
  zonesLoading: boolean
  errors: WebsiteFormErrors
  onHostnameChange: (hostname: string) => void
}) {
  const [chosen, setChosen] = useState<{ zoneId: string; prefix: string }>()
  const split = splitHostedDomain(domain.hostname, zones)
  const selection =
    chosen && zones.some((zone) => zone.id === chosen.zoneId)
      ? chosen
      : split
        ? { zoneId: split.zone.id, prefix: split.prefix }
        : { zoneId: '', prefix: chosen?.prefix ?? domain.hostname }
  const zone = zones.find((item) => item.id === selection.zoneId)
  const owner = findHostedZone(domain.hostname, zones)

  function select(zoneId: string, prefix: string) {
    setChosen({ zoneId, prefix })
    onHostnameChange(
      composeHostedDomain(
        prefix,
        zones.find((item) => item.id === zoneId)?.domain ?? ''
      )
    )
  }

  const state = zone && hostedZoneState(zone)
  const nested = zone && owner && owner.id !== zone.id ? owner : undefined

  return (
    <div className='flex min-w-0 flex-1 flex-col gap-1.5'>
      <div className='grid grid-cols-[minmax(6rem,2fr)_auto_minmax(0,5fr)] items-start gap-x-1.5 sm:grid-cols-[minmax(0,2fr)_auto_minmax(0,3fr)]'>
        <Field
          label='主机前缀'
          value={selection.prefix}
          onChange={(prefix) => {
            // A full hostname typed or pasted here is re-split by its zone.
            const pasted = splitHostedDomain(prefix, zones)
            if (pasted) select(pasted.zone.id, pasted.prefix)
            else select(selection.zoneId, prefix)
          }}
          error={errors[`domains.${index}.hostname`]}
          placeholder='@ 根域名'
          disabled={zonesLoading}
          className='[&_input]:font-mono'
        />
        <span
          aria-hidden
          className='mt-6 flex h-9 items-center font-mono text-base text-muted'
        >
          .
        </span>
        <Choice
          label='托管域名'
          value={selection.zoneId}
          onChange={(zoneId) => select(zoneId, selection.prefix)}
          items={zones.map((item) => ({
            id: item.id,
            label: hostedZoneLabel(item),
          }))}
          disabled={zonesLoading}
          error={errors[`domains.${index}.zone`]}
          // Long `domain · provider` labels stay on one line in the trigger.
          className='[&_[data-slot=select-value]]:min-w-0 [&_[data-slot=select-value]]:truncate'
        />
        {state && (
          <p
            className={cn(
              'col-start-3 mt-1.5 inline-flex items-center gap-1.5 text-xs',
              state.danger ? 'text-danger' : 'text-warning'
            )}
          >
            <span
              aria-hidden
              className={cn(
                'size-1.5 rounded-full',
                state.danger ? 'bg-danger' : 'bg-warning'
              )}
            />
            {state.label}
          </p>
        )}
      </div>
      {nested && (
        <p className='text-xs text-muted'>
          记录写入更具体的托管域名 <Mono>{nested.domain}</Mono>
        </p>
      )}
    </div>
  )
}

export function DomainsSection({
  values,
  errors,
  update,
  website,
}: WebsiteSectionProps) {
  const clusters = useQuery(clusterOptionsQuery)
  const hasManaged = values.domains.some(
    (domain) => domain.dns_mode === 'managed'
  )
  const zones = useQuery(hostedZoneOptionsQuery(hasManaged))
  const accessDomain =
    clusters.data?.find((item) => item.id === values.cluster_id)
      ?.access_domain ?? website?.access_domain
  function patchDomain(id: string, patch: Partial<WebsiteDomain>) {
    update(
      'domains',
      values.domains.map((domain) =>
        domain.id === id ? { ...domain, ...patch } : domain
      )
    )
  }
  return (
    <Section
      title='域名'
      description={
        accessDomain ? (
          <>
            外部 DNS：将域名 CNAME 到 <Mono>{accessDomain}</Mono>
          </>
        ) : undefined
      }
      actions={
        <>
          {hasManaged && (
            <>
              <NewWindowLink href='/dns-zones?create=1'>
                添加托管域名
              </NewWindowLink>
              <IconAction
                label='刷新托管域名'
                icon={<ArrowRotateLeft className='size-3.5' />}
                isPending={zones.isFetching}
                onPress={() => void zones.refetch()}
              />
            </>
          )}
          <Button
            size='sm'
            variant='tertiary'
            isDisabled={values.domains.length >= 100}
            onPress={() =>
              update('domains', [
                ...values.domains,
                { id: crypto.randomUUID(), hostname: '', dns_mode: 'managed' },
              ])
            }
          >
            <Plus className='size-4' />
            添加域名
          </Button>
        </>
      }
    >
      <div className='flex flex-col gap-3'>
        {hasManaged && (
          <>
            <QueryNotice
              query={zones}
              title='托管域名加载失败'
              onRetry={() => void zones.refetch()}
            />
            {zones.isSuccess && zones.data.length === 0 && (
              <p className='text-[13px] text-muted'>
                还没有托管域名，请先在“域名管理”中添加
              </p>
            )}
          </>
        )}
        {values.domains.map((domain, index) => (
          <div
            key={domain.id}
            className='flex flex-col gap-3 rounded-lg border border-border p-3'
          >
            <div className='flex items-center justify-between gap-2'>
              <p className='text-sm font-medium'>域名 {index + 1}</p>
              <IconAction
                label={`移除域名 ${index + 1}`}
                icon={<TrashBin className='size-4' />}
                onPress={() =>
                  update(
                    'domains',
                    values.domains.filter((item) => item.id !== domain.id)
                  )
                }
              />
            </div>
            <div className='flex flex-col gap-3 sm:flex-row sm:items-start'>
              <Choice
                label='DNS 模式'
                value={domain.dns_mode}
                items={dnsModeItems}
                onChange={(mode) =>
                  patchDomain(domain.id, {
                    dns_mode: mode as WebsiteDomain['dns_mode'],
                  })
                }
                className='sm:w-40 sm:shrink-0'
              />
              {domain.dns_mode === 'managed' ? (
                <HostedDomainFields
                  domain={domain}
                  index={index}
                  zones={zones.data ?? []}
                  zonesLoading={zones.isPending}
                  errors={errors}
                  onHostnameChange={(hostname) =>
                    patchDomain(domain.id, { hostname })
                  }
                />
              ) : (
                <Field
                  label='完整域名'
                  value={domain.hostname}
                  onChange={(hostname) => patchDomain(domain.id, { hostname })}
                  error={errors[`domains.${index}.hostname`]}
                  placeholder='www.example.com'
                  className='min-w-0 flex-1 [&_input]:font-mono'
                />
              )}
            </div>
          </div>
        ))}
        {values.domains.length === 0 && (
          <p className='py-2 text-sm text-muted'>
            还没有域名，至少添加一个域名才能保存。
          </p>
        )}
        <FieldNotice>{errors.domains}</FieldNotice>
      </div>
    </Section>
  )
}

export function HttpsSection({
  values,
  errors,
  update,
  replace,
  website,
}: WebsiteSectionProps) {
  const certificates = useQuery({
    ...usableCertificateOptionsQuery,
    enabled: values.https_enabled,
    // QueryNotice below shows failures; no duplicate toast.
    meta: { inlineError: true },
  })
  const hostnames = websiteHostnames(values)
  const options = websiteCertificateOptions(
    certificates.data ?? [],
    values.certificate_ids,
    website?.certificates ?? [],
    hostnames
  )
  const selectedUsable = options.filter(
    (certificate) =>
      certificate.usable && values.certificate_ids.includes(certificate.id)
  )
  const uncovered = selectedUsable.length
    ? uncoveredHostnames(selectedUsable, hostnames)
    : []
  const expiry = new Map(
    (certificates.data ?? []).map((item) => [item.id, item.expires_at])
  )
  return (
    <>
      <Section title='HTTPS' description='启用后客户端可通过 TLS 访问网站。'>
        <Toggle
          label='启用 HTTPS'
          hint='关闭时会同时解除证书绑定、HTTPS 跳转和 HSTS。'
          selected={values.https_enabled}
          onChange={(enabled) =>
            replace((current) => setWebsiteHttpsEnabled(current, enabled))
          }
        />
      </Section>
      {values.https_enabled && (
        <>
          <Section
            title='绑定证书'
            description='可选择多张可用证书覆盖网站域名。'
            actions={
              <>
                <NewWindowLink href='/certificates?create=1'>
                  申请证书
                </NewWindowLink>
                <RefreshOptions
                  label='刷新证书'
                  isPending={certificates.isFetching}
                  onRefresh={() => void certificates.refetch()}
                />
              </>
            }
          >
            <div className='flex flex-col gap-3'>
              <QueryNotice
                query={certificates}
                title='证书列表加载失败'
                onRetry={() => void certificates.refetch()}
              />
              {options.length > 0 && (
                <ul className='divide-y divide-border rounded-xl border border-border'>
                  {options.map((certificate) => (
                    <li
                      key={certificate.id}
                      className='flex flex-wrap items-center gap-x-4 gap-y-1 px-3 py-2'
                    >
                      <CheckOption
                        selected={values.certificate_ids.includes(
                          certificate.id
                        )}
                        onChange={(selected) =>
                          update(
                            'certificate_ids',
                            selected
                              ? [...values.certificate_ids, certificate.id]
                              : values.certificate_ids.filter(
                                  (id) => id !== certificate.id
                                )
                          )
                        }
                        label={
                          <Mono className='break-all'>
                            {certificate.domains.join('、') || certificate.id}
                          </Mono>
                        }
                      />
                      {certificate.usable && !certificate.matched ? (
                        <span className='text-xs text-danger'>
                          未匹配网站域名
                        </span>
                      ) : (
                        <span className='text-xs text-muted'>
                          {certificate.usable
                            ? expiry.get(certificate.id)
                              ? `到期 ${formatDate(expiry.get(certificate.id))}`
                              : '可用'
                            : certificates.isPending || certificates.isError
                              ? '正在读取可用证书，当前绑定仍可移除'
                              : '证书不可用，请取消绑定并选择可用证书'}
                        </span>
                      )}
                      <span className='ml-auto'>
                        <NewWindowLink href={`/certificates/${certificate.id}`}>
                          详情
                        </NewWindowLink>
                      </span>
                    </li>
                  ))}
                </ul>
              )}
              {!certificates.isPending && options.length === 0 && (
                <p className='py-2 text-sm text-muted'>
                  {hostnames.length
                    ? '暂无匹配网站域名的可用证书。'
                    : '请先绑定域名'}
                </p>
              )}
              {uncovered.length > 0 && (
                <p className='text-sm text-warning'>
                  证书未覆盖：{uncovered.join('、')}
                </p>
              )}
              <FieldNotice>{errors.certificate_ids}</FieldNotice>
            </div>
          </Section>
          <Section title='TLS 与跳转'>
            <div className='flex flex-col gap-2'>
              <div className='max-w-xs'>
                <Choice
                  label='最低 TLS 版本'
                  value={values.minimum_tls_version}
                  onChange={(version) =>
                    update('minimum_tls_version', version as '1.2' | '1.3')
                  }
                  items={[
                    { id: '1.2', label: 'TLS 1.2' },
                    { id: '1.3', label: 'TLS 1.3' },
                  ]}
                />
              </div>
              <Toggle
                label='HTTP 自动跳转 HTTPS'
                selected={values.force_https}
                onChange={(selected) => update('force_https', selected)}
              />
              <Toggle
                label='启用 HSTS'
                hint='通知浏览器后续仅通过 HTTPS 访问。'
                selected={values.hsts_enabled}
                onChange={(selected) => update('hsts_enabled', selected)}
              />
            </div>
          </Section>
        </>
      )}
    </>
  )
}

function OriginGroupList({
  values,
  replace,
  onProblem,
}: Pick<WebsiteSectionProps, 'values' | 'replace'> & {
  onProblem: (message: string) => void
}) {
  const [renaming, setRenaming] = useState<string | null>(null)
  const [draft, setDraft] = useState('')
  const [renameError, setRenameError] = useState('')
  const groups = collectOriginGroups(values.origins)
  return (
    <ul className='divide-y divide-border rounded-xl border border-border'>
      {groups.map((group) => (
        <li key={group.name} className='flex flex-col gap-2 px-3 py-2'>
          {renaming === group.name ? (
            <div className='flex flex-wrap items-start gap-2'>
              <div className='min-w-48 flex-1'>
                <Field
                  label='源站组新名称'
                  value={draft}
                  onChange={setDraft}
                  error={renameError}
                />
              </div>
              <div className='flex gap-2 sm:pt-6'>
                <Button
                  size='sm'
                  variant='tertiary'
                  onPress={() => {
                    const result = renameOriginGroup(values, group.name, draft)
                    if (!result.ok) {
                      setRenameError(result.message)
                      return
                    }
                    replace((current) => ({ ...current, ...result.value }))
                    setRenaming(null)
                    onProblem('')
                  }}
                >
                  应用
                </Button>
                <Button
                  size='sm'
                  variant='tertiary'
                  onPress={() => setRenaming(null)}
                >
                  取消
                </Button>
              </div>
            </div>
          ) : (
            <div className='flex flex-wrap items-center gap-x-3 gap-y-1'>
              <span className='text-sm font-medium'>{group.name}</span>
              {values.default_origin_group.trim() === group.name && (
                <Chip size='sm' variant='soft' color='accent'>
                  默认
                </Chip>
              )}
              <span className='text-xs text-muted tabular-nums'>
                {group.indexes.length} 个源站 · {group.enabledPrimaryCount} 主源
                / {group.enabledCount} 启用
              </span>
              <span className='ml-auto'>
                <IconAction
                  label={`重命名源站组 ${group.name}`}
                  icon={<Pencil className='size-4' />}
                  onPress={() => {
                    setRenaming(group.name)
                    setDraft(group.name)
                    setRenameError('')
                  }}
                />
              </span>
            </div>
          )}
        </li>
      ))}
      {groups.length === 0 && (
        <li className='px-3 py-3 text-sm text-muted'>
          还没有源站组，添加源站时填写源站组名称。
        </li>
      )}
    </ul>
  )
}

function newOrigin(group: string): WebsiteOrigin {
  return {
    id: crypto.randomUUID(),
    group,
    protocol: 'http',
    host: '',
    port: 80,
    role: 'primary',
    weight: 100,
    status: 'enabled',
  }
}

export function OriginsSection({
  values,
  errors,
  update,
  replace,
}: WebsiteSectionProps) {
  const [problem, setProblem] = useState('')
  const groups = collectOriginGroups(values.origins)
  function patchOrigin(id: string, patch: Partial<WebsiteOrigin>) {
    update(
      'origins',
      values.origins.map((origin) =>
        origin.id === id ? { ...origin, ...patch } : origin
      )
    )
  }
  return (
    <>
      <Section
        title='源站组'
        description='请求默认发往默认源站组；路由规则可把请求转发到其他源站组。'
      >
        <div className='flex flex-col gap-4'>
          <div className='max-w-sm'>
            <Choice
              label='默认源站组'
              value={values.default_origin_group}
              onChange={(group) => update('default_origin_group', group)}
              items={groups.map((group) => ({
                id: group.name,
                label: group.name,
              }))}
              error={errors.default_origin_group}
            />
          </div>
          <OriginGroupList
            values={values}
            replace={replace}
            onProblem={setProblem}
          />
        </div>
      </Section>
      <Section
        title='源站'
        description='同一源站组中的主源按权重分发，主源都不可用时使用备用源站。'
        actions={
          <>
            <Button
              size='sm'
              variant='tertiary'
              isDisabled={values.origins.length >= 100}
              onPress={() =>
                update('origins', [
                  ...values.origins,
                  newOrigin(nextOriginGroupName(groups)),
                ])
              }
            >
              <Plus className='size-4' />
              添加源站组
            </Button>
            <Button
              size='sm'
              variant='tertiary'
              isDisabled={values.origins.length >= 100}
              onPress={() =>
                update('origins', [
                  ...values.origins,
                  newOrigin(values.default_origin_group || 'default'),
                ])
              }
            >
              <Plus className='size-4' />
              添加源站
            </Button>
          </>
        }
      >
        <div className='flex flex-col gap-3'>
          <Notice>{problem}</Notice>
          {values.origins.map((origin, index) => (
            <div
              key={origin.id}
              role='group'
              aria-label={`源站 ${index + 1}`}
              className='flex flex-col gap-3 rounded-xl border border-border p-3 sm:p-4'
            >
              <div className='flex items-center justify-between gap-3'>
                <h3 className='min-w-0 truncate text-sm font-medium'>
                  源站 {index + 1}
                  {origin.host && (
                    <Mono className='ml-2 text-muted'>
                      {origin.protocol}://{origin.host}:{origin.port}
                    </Mono>
                  )}
                </h3>
                <div className='flex shrink-0 items-center gap-3 whitespace-nowrap'>
                  <Toggle
                    label='启用'
                    selected={origin.status === 'enabled'}
                    onChange={(enabled) =>
                      patchOrigin(origin.id, {
                        status: enabled ? 'enabled' : 'disabled',
                      })
                    }
                  />
                  <IconAction
                    label={`移除源站 ${index + 1}`}
                    icon={<TrashBin className='size-4' />}
                    onPress={() => {
                      const removal = originRemovalError(values, index)
                      if (removal) {
                        setProblem(removal)
                        return
                      }
                      setProblem('')
                      update(
                        'origins',
                        values.origins.filter((item) => item.id !== origin.id)
                      )
                    }}
                  />
                </div>
              </div>
              <div className='grid gap-3 sm:grid-cols-2 xl:grid-cols-[minmax(0,1fr)_minmax(0,1.5fr)_7rem_7rem_8rem_6rem]'>
                <Field
                  label='源站组'
                  value={origin.group}
                  onChange={(group) => patchOrigin(origin.id, { group })}
                  error={errors[`origins.${index}.group`]}
                />
                <Field
                  label='主机 / IP'
                  value={origin.host}
                  onChange={(host) => patchOrigin(origin.id, { host })}
                  error={errors[`origins.${index}.host`]}
                  placeholder='origin.example.com'
                  className='[&_input]:font-mono'
                />
                <Choice
                  label='协议'
                  value={origin.protocol}
                  onChange={(protocol) =>
                    patchOrigin(origin.id, {
                      protocol: protocol as WebsiteOrigin['protocol'],
                    })
                  }
                  items={[
                    { id: 'http', label: 'HTTP' },
                    { id: 'https', label: 'HTTPS' },
                  ]}
                />
                <NumberField
                  label='端口'
                  value={origin.port}
                  onChange={(port) => patchOrigin(origin.id, { port })}
                  error={errors[`origins.${index}.port`]}
                  min={1}
                  max={65535}
                />
                <Choice
                  label='角色'
                  value={origin.role}
                  onChange={(role) =>
                    patchOrigin(origin.id, {
                      role: role as WebsiteOrigin['role'],
                    })
                  }
                  items={[
                    { id: 'primary', label: '主源站' },
                    { id: 'backup', label: '备用源站' },
                  ]}
                />
                <NumberField
                  label='权重'
                  value={origin.weight}
                  onChange={(weight) => patchOrigin(origin.id, { weight })}
                  error={errors[`origins.${index}.weight`]}
                  min={1}
                  max={100}
                />
              </div>
            </div>
          ))}
          {values.origins.length === 0 && (
            <p className='py-2 text-sm text-muted'>
              还没有源站，至少添加一个源站才能保存。
            </p>
          )}
          <FieldNotice>{errors.origins}</FieldNotice>
        </div>
      </Section>
    </>
  )
}

export function OriginSettingsSection({
  values,
  errors,
  update,
}: WebsiteSectionProps) {
  return (
    <Section title='回源' description='边缘节点向源站发起请求时使用的参数。'>
      <div className='flex flex-col gap-4'>
        <div className='grid gap-4 sm:grid-cols-2'>
          <Field
            label='回源 Host'
            value={values.origin_host_header}
            onChange={(host) => update('origin_host_header', host)}
            error={errors.origin_host_header}
            hint='$host 保留客户端请求域名，也可指定源站域名。'
            className='[&_input]:font-mono'
          />
          <div className='hidden sm:block' />
          <NumberField
            label='连接超时（秒）'
            value={values.origin_connect_timeout_seconds}
            onChange={(value) =>
              update('origin_connect_timeout_seconds', value)
            }
            error={errors.origin_connect_timeout_seconds}
            hint='1–300 秒。'
            min={1}
            max={300}
          />
          <NumberField
            label='读取超时（秒）'
            value={values.origin_read_timeout_seconds}
            onChange={(value) => update('origin_read_timeout_seconds', value)}
            error={errors.origin_read_timeout_seconds}
            hint='1–600 秒。'
            min={1}
            max={600}
          />
        </div>
        <Toggle
          label='传递客户端 IP'
          hint='通过转发头把客户端地址传给源站。'
          selected={values.pass_client_ip}
          onChange={(selected) => update('pass_client_ip', selected)}
        />
      </div>
    </Section>
  )
}

export function HealthSection({ values, errors, update }: WebsiteSectionProps) {
  return (
    <Section
      title='健康检查'
      description='边缘节点定期探测源站，连续失败的源站会暂停接收请求。'
    >
      <div className='flex flex-col gap-4'>
        <Toggle
          label='主动健康检查'
          selected={values.health_check_enabled}
          onChange={(selected) => update('health_check_enabled', selected)}
        />
        {values.health_check_enabled && (
          <div className='grid gap-4 sm:grid-cols-2 xl:grid-cols-3'>
            <Field
              label='检查路径'
              value={values.health_check_path}
              onChange={(path) => update('health_check_path', path)}
              error={errors.health_check_path}
              className='[&_input]:font-mono'
            />
            <NumberField
              label='预期状态码'
              value={values.health_check_expected_status}
              onChange={(value) =>
                update('health_check_expected_status', value)
              }
              error={errors.health_check_expected_status}
              min={100}
              max={599}
            />
            <NumberField
              label='检查间隔（秒）'
              value={values.health_check_interval_seconds}
              onChange={(value) =>
                update('health_check_interval_seconds', value)
              }
              error={errors.health_check_interval_seconds}
              min={1}
              max={3600}
            />
            <NumberField
              label='检查超时（秒）'
              value={values.health_check_timeout_seconds}
              onChange={(value) =>
                update('health_check_timeout_seconds', value)
              }
              error={errors.health_check_timeout_seconds}
              min={1}
              max={300}
            />
            <NumberField
              label='恢复健康阈值'
              value={values.healthy_threshold}
              onChange={(value) => update('healthy_threshold', value)}
              error={errors.healthy_threshold}
              hint='连续成功次数，1–10。'
              min={1}
              max={10}
            />
            <NumberField
              label='判定失败阈值'
              value={values.unhealthy_threshold}
              onChange={(value) => update('unhealthy_threshold', value)}
              error={errors.unhealthy_threshold}
              hint='连续失败次数，1–10。'
              min={1}
              max={10}
            />
          </div>
        )}
      </div>
    </Section>
  )
}

const compressionAlgorithms = ['br', 'zstd', 'gzip'] as const

export function CompressionSection({
  values,
  errors,
  update,
}: WebsiteSectionProps) {
  const algorithms = values.response_compression_algorithms
  function moveAlgorithm(index: number, direction: -1 | 1) {
    const next = [...algorithms]
    ;[next[index], next[index + direction]] = [
      next[index + direction],
      next[index],
    ]
    update('response_compression_algorithms', next)
  }
  return (
    <Section title='响应压缩' description='按客户端支持的算法压缩文本类响应。'>
      <div className='flex flex-col gap-4'>
        <Toggle
          label='启用响应压缩'
          selected={values.response_compression_enabled}
          onChange={(selected) =>
            update('response_compression_enabled', selected)
          }
        />
        {values.response_compression_enabled && (
          <>
            <div className='grid gap-4 sm:grid-cols-2'>
              <NumberField
                label='最小响应大小（字节）'
                value={values.response_compression_min_bytes}
                onChange={(value) =>
                  update('response_compression_min_bytes', value)
                }
                error={errors.response_compression_min_bytes}
                hint='256–1048576 字节。'
                min={256}
                max={1048576}
              />
              <NumberField
                label='最大响应大小（字节）'
                value={values.response_compression_max_bytes}
                onChange={(value) =>
                  update('response_compression_max_bytes', value)
                }
                error={errors.response_compression_max_bytes}
                hint='0 表示不限制。'
                min={0}
                max={67108864}
              />
            </div>
            <div className='flex flex-col gap-2'>
              <div>
                <h3 className='text-sm font-medium'>压缩算法优先级</h3>
                <p className='text-xs text-muted'>
                  按列表顺序选择客户端支持的第一个算法。
                </p>
              </div>
              <ol className='divide-y divide-border rounded-xl border border-border'>
                {algorithms.map((algorithm, index) => (
                  <li
                    key={algorithm}
                    className='flex items-center gap-2 px-3 py-1.5'
                  >
                    <span className='w-5 text-xs text-muted tabular-nums'>
                      {index + 1}
                    </span>
                    <Mono className='flex-1'>{algorithm}</Mono>
                    <IconAction
                      label={`上移 ${algorithm}`}
                      icon={<ArrowUp className='size-4' />}
                      isDisabled={index === 0}
                      onPress={() => moveAlgorithm(index, -1)}
                    />
                    <IconAction
                      label={`下移 ${algorithm}`}
                      icon={<ArrowDown className='size-4' />}
                      isDisabled={index === algorithms.length - 1}
                      onPress={() => moveAlgorithm(index, 1)}
                    />
                    <IconAction
                      label={`移除 ${algorithm}`}
                      icon={<TrashBin className='size-4' />}
                      onPress={() =>
                        update(
                          'response_compression_algorithms',
                          algorithms.filter((item) => item !== algorithm)
                        )
                      }
                    />
                  </li>
                ))}
                {algorithms.length === 0 && (
                  <li className='px-3 py-2 text-sm text-muted'>
                    尚未选择压缩算法。
                  </li>
                )}
              </ol>
              <div className='flex flex-wrap gap-2'>
                {compressionAlgorithms
                  .filter((algorithm) => !algorithms.includes(algorithm))
                  .map((algorithm) => (
                    <Button
                      key={algorithm}
                      size='sm'
                      variant='tertiary'
                      onPress={() =>
                        update('response_compression_algorithms', [
                          ...algorithms,
                          algorithm,
                        ])
                      }
                    >
                      <Plus className='size-4' />
                      添加 {algorithm}
                    </Button>
                  ))}
              </div>
              <FieldNotice>
                {errors.response_compression_algorithms}
              </FieldNotice>
            </div>
            <div className='grid gap-4 lg:grid-cols-3'>
              <ArrayLines
                label='MIME 类型'
                value={values.response_compression_mime_types}
                onChange={(items) =>
                  update('response_compression_mime_types', items)
                }
                hint='每行一项，可使用 text/*。'
              />
              <ArrayLines
                label='扩展名'
                value={values.response_compression_extensions}
                onChange={(items) =>
                  update('response_compression_extensions', items)
                }
                hint='每行一项，如 .js。'
              />
              <ArrayLines
                label='排除扩展名'
                value={values.response_compression_excluded_extensions}
                onChange={(items) =>
                  update('response_compression_excluded_extensions', items)
                }
                hint='命中的扩展名不压缩。'
              />
            </div>
          </>
        )}
      </div>
    </Section>
  )
}

const logFields = [
  ['access_log_request_headers', '请求头'],
  ['access_log_response_headers', '响应头'],
  ['access_log_request_body', '请求体'],
  ['access_log_query_params', '查询参数'],
  ['access_log_cookies', 'Cookie'],
  ['access_log_referer', 'Referer'],
  ['access_log_user_agent', 'User-Agent'],
  ['access_log_client_abort', '客户端中断'],
] as const

const statusRanges = ['1xx', '2xx', '3xx', '4xx', '5xx'] as const

export function LogsSection({ values, errors, update }: WebsiteSectionProps) {
  return (
    <Section
      title='访问日志'
      description='记录的日志可在网站的访问日志中查看，并计入流量统计。'
    >
      <div className='flex flex-col gap-4'>
        <Toggle
          label='记录访问日志'
          selected={values.access_log_enabled}
          onChange={(selected) => update('access_log_enabled', selected)}
        />
        {values.access_log_enabled && (
          <>
            <div role='group' aria-labelledby='access-log-fields'>
              <h3 id='access-log-fields' className='text-sm font-medium'>
                记录内容
              </h3>
              <div className='grid gap-x-6 sm:grid-cols-2 xl:grid-cols-4'>
                {logFields.map(([key, label]) => (
                  <CheckOption
                    key={key}
                    label={label}
                    selected={values[key]}
                    onChange={(selected) => update(key, selected)}
                  />
                ))}
              </div>
            </div>
            <div role='group' aria-labelledby='access-log-status'>
              <h3 id='access-log-status' className='text-sm font-medium'>
                记录的状态码
              </h3>
              <div className='flex flex-wrap gap-x-6'>
                {statusRanges.map((range) => (
                  <CheckOption
                    key={range}
                    label={<Mono>{range}</Mono>}
                    selected={values.access_log_status_code_ranges.includes(
                      range
                    )}
                    onChange={(selected) =>
                      update(
                        'access_log_status_code_ranges',
                        selected
                          ? [...values.access_log_status_code_ranges, range]
                          : values.access_log_status_code_ranges.filter(
                              (item) => item !== range
                            )
                      )
                    }
                  />
                ))}
              </div>
              <FieldNotice>{errors.access_log_status_code_ranges}</FieldNotice>
            </div>
          </>
        )}
      </div>
    </Section>
  )
}
