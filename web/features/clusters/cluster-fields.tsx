import { Controller, useWatch, type UseFormReturn } from 'react-hook-form'
import { useQuery } from '@tanstack/react-query'
import { Choice, Field } from '@/components/forms'
import { Mono } from '@/components/mono'
import { QueryNotice } from '@/components/page'
import { dnsZoneOptionsQuery } from '@/features/dns-zones/data'
import type { ClusterFormValues } from './cluster-form'
import type { Cluster } from './types'

type FieldsProps = {
  form: UseFormReturn<ClusterFormValues>
  disabled: boolean
}

/** 集群名称 and 启用状态, shared by the create sheet and the 配置 tab. */
export function ClusterBasicFields({ form, disabled }: FieldsProps) {
  return (
    <>
      <Controller
        control={form.control}
        name='name'
        render={({ field, fieldState }) => (
          <Field
            label='集群名称'
            value={field.value}
            onChange={field.onChange}
            error={fieldState.error?.message}
            required
            disabled={disabled}
            placeholder='华东边缘集群'
          />
        )}
      />
      <Controller
        control={form.control}
        name='status'
        render={({ field }) => (
          <Choice
            label='启用状态'
            value={field.value}
            onChange={field.onChange}
            disabled={disabled}
            items={[
              { id: 'enabled', label: '已启用' },
              { id: 'disabled', label: '已停用' },
            ]}
          />
        )}
      />
    </>
  )
}

/** 托管域名, 主机前缀 and the `<prefix>.<zone>` access domain preview. The
 * zone options are read once per mount while `enabled`; the saved zone of
 * `cluster` stays selectable even when no longer available. */
export function ClusterAccessFields({
  form,
  disabled,
  cluster,
  enabled = true,
}: FieldsProps & { cluster?: Cluster; enabled?: boolean }) {
  const zones = useQuery({ ...dnsZoneOptionsQuery, enabled, staleTime: 0 })
  const zoneId = useWatch({ control: form.control, name: 'dns_zone_id' })
  const prefix = useWatch({ control: form.control, name: 'hostname_prefix' })
  const zoneItems = (zones.data ?? [])
    .filter((zone) => zone.available || zone.id === cluster?.dns_zone_id)
    .map((zone) => ({
      id: zone.id,
      label: `${zone.domain} · ${zone.dns_provider_name}`,
      domain: zone.domain,
    }))
  if (cluster && !zoneItems.some((zone) => zone.id === cluster.dns_zone_id))
    zoneItems.push({
      id: cluster.dns_zone_id,
      label: cluster.dns_zone_domain,
      domain: cluster.dns_zone_domain,
    })
  const selectedZone = zoneItems.find((zone) => zone.id === zoneId)
  // The options endpoint marks a zone available once it is synced and its
  // DNS account verified; say why the picker is empty instead of a silent list.
  const zoneHint =
    zones.isSuccess && zoneItems.length === 0
      ? zones.data.length === 0
        ? '还没有托管域名，请先在“域名管理”中添加'
        : '暂无可用托管域名：域名同步完成且所属账号验证通过后才可选择'
      : '只列出已同步且账号已验证的托管域名'
  return (
    <>
      <QueryNotice
        query={zones}
        title='托管域名加载失败'
        onRetry={() => void zones.refetch()}
      />
      <Controller
        control={form.control}
        name='dns_zone_id'
        render={({ field, fieldState }) => (
          <Choice
            label='托管域名'
            value={field.value}
            onChange={field.onChange}
            items={zoneItems.map(({ id, label }) => ({ id, label }))}
            required
            error={fieldState.error?.message}
            hint={zoneHint}
            disabled={disabled || zones.isPending}
          />
        )}
      />
      <Controller
        control={form.control}
        name='hostname_prefix'
        render={({ field, fieldState }) => (
          <Field
            label='主机前缀'
            value={field.value}
            onChange={field.onChange}
            error={fieldState.error?.message}
            required
            disabled={disabled}
            placeholder='edge'
          />
        )}
      />
      <div className='flex flex-col gap-1 rounded-lg border border-border bg-surface-secondary p-3'>
        <p className='text-xs text-muted'>接入域名</p>
        <Mono className='break-all'>
          {prefix.trim() && selectedZone
            ? `${prefix.trim()}.${selectedZone.domain}`
            : '填写主机前缀并选择托管域名后生成'}
        </Mono>
      </div>
    </>
  )
}
