import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { ArrowRotateLeft, Plus, TrashBin } from '@gravity-ui/icons'
import { Button } from '@heroui/react'
import { cn } from '@/lib/utils'
import { Choice, Field } from '@/components/forms'
import { Mono } from '@/components/mono'
import { QueryNotice } from '@/components/page'
import { Section } from '@/components/section'
import { clusterOptionsQuery } from '@/features/clusters/data'
import type { DnsZoneOption } from '@/features/dns-zones/types'
import { hostedZoneOptionsQuery } from './data'
import {
  composeHostedDomain,
  findHostedZone,
  splitHostedDomain,
} from './hosted-domain'
import { FieldNotice, IconAction } from './settings-controls'
import type { WebsiteFormValues } from './website-form'
import type { WebsiteFormErrors, WebsiteSectionProps } from './website-sections'

type WebsiteDomain = WebsiteFormValues['domains'][number]

const dnsModeItems = [
  { id: 'managed', label: '托管 DNS' },
  { id: 'external', label: '外部 DNS' },
]

/** Why a zone is not ready yet; every zone stays selectable regardless. */
function hostedZoneState(zone: DnsZoneOption) {
  if (zone.available) return undefined
  if (zone.sync_status === 'failed') return { label: '同步异常', danger: true }
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

function ExternalDomainFields({
  domain,
  index,
  errors,
  onHostnameChange,
}: {
  domain: WebsiteDomain
  index: number
  errors: WebsiteFormErrors
  onHostnameChange: (hostname: string) => void
}) {
  return (
    <Field
      label='完整域名'
      value={domain.hostname}
      onChange={onHostnameChange}
      error={errors[`domains.${index}.hostname`]}
      placeholder='www.example.com'
      className='min-w-0 flex-1 [&_input]:font-mono'
    />
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
            <IconAction
              label='刷新托管域名'
              icon={<ArrowRotateLeft className='size-3.5' />}
              isPending={zones.isFetching}
              onPress={() => void zones.refetch()}
            />
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
                还没有托管域名，请先在“域名”中添加
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
                <ExternalDomainFields
                  domain={domain}
                  index={index}
                  errors={errors}
                  onHostnameChange={(hostname) =>
                    patchDomain(domain.id, { hostname })
                  }
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
