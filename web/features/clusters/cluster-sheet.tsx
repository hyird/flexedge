import { useEffect, useEffectEvent, useState } from 'react'
import { Controller, useForm, useWatch } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery } from '@tanstack/react-query'
import { toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { FormSheet } from '@/components/form-sheet'
import { Choice, Field } from '@/components/forms'
import { Mono } from '@/components/mono'
import { QueryNotice } from '@/components/page'
import { dnsZoneOptionsQuery } from '@/features/dns-zones/data'
import {
  clusterFormSchema,
  clusterFormValues,
  type ClusterFormValues,
} from './cluster-form'
import { saveCluster } from './data'
import type { Cluster } from './types'

/** Create (no `cluster`) or edit a cluster in a right sheet. The access
 * domain preview is `<prefix>.<zone>`. */
export function ClusterSheet({
  isOpen,
  cluster,
  onClose,
  onSaved,
}: {
  isOpen: boolean
  cluster?: Cluster
  onClose: () => void
  onSaved?: () => void
}) {
  // One snapshot per opening: the always-mounted sheet keeps its observer
  // while closed, so only staleness makes re-enabling read again.
  const zones = useQuery({
    ...dnsZoneOptionsQuery,
    enabled: isOpen,
    staleTime: 0,
  })
  const form = useForm<ClusterFormValues>({
    resolver: zodResolver(clusterFormSchema),
    defaultValues: clusterFormValues(cluster),
  })
  // Each opening starts from the target's saved values and pins the revision
  // the edit is based on; later live snapshots must not overwrite the draft.
  const target = isOpen ? (cluster?.id ?? 'new') : null
  const [openedFor, setOpenedFor] = useState(target)
  const [base, setBase] = useState(cluster)
  if (target !== openedFor) {
    setOpenedFor(target)
    if (target) setBase(cluster)
  }
  const resetForm = useEffectEvent(() => form.reset(clusterFormValues(cluster)))
  useEffect(() => {
    if (target) resetForm()
  }, [target])
  const zoneId = useWatch({ control: form.control, name: 'dns_zone_id' })
  const prefix = useWatch({ control: form.control, name: 'hostname_prefix' })
  const selectedZone = zones.data?.find((zone) => zone.id === zoneId)
  const mutation = useMutation({
    meta: { inlineError: true },
    mutationFn: (values: ClusterFormValues) => saveCluster(values, base),
    onSuccess: (response) => {
      toast.success(response.message)
      onSaved?.()
      onClose()
    },
  })
  const zoneItems = (zones.data ?? [])
    .filter((zone) => zone.available || zone.id === cluster?.dns_zone_id)
    .map((zone) => ({
      id: zone.id,
      label: `${zone.domain} · ${zone.dns_provider_name}`,
    }))
  if (cluster && !zoneItems.some((zone) => zone.id === cluster.dns_zone_id))
    zoneItems.push({ id: cluster.dns_zone_id, label: cluster.dns_zone_domain })
  // The options endpoint marks a zone available once it is synced and its
  // DNS account verified; say why the picker is empty instead of a silent list.
  const zoneHint =
    zones.isSuccess && zoneItems.length === 0
      ? zones.data.length === 0
        ? '还没有 DNS 区域，请先在“DNS 区域”中添加'
        : '暂无可用托管域名：DNS 区域同步完成且所属账号验证通过后才可选择'
      : '只列出已同步且账号已验证的托管域名'
  return (
    <FormSheet
      isOpen={isOpen}
      onClose={() => {
        mutation.reset()
        onClose()
      }}
      title={cluster ? `编辑集群 ${cluster.name}` : '创建集群'}
      description='接入域名由主机前缀和托管域名组合生成，网站通过 CNAME 指向它接入集群节点。'
      onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
      submitLabel={cluster ? '保存集群' : '创建集群'}
      isSubmitting={mutation.isPending}
      error={mutation.isError ? apiErrorMessage(mutation.error) : undefined}
    >
      <QueryNotice
        query={zones}
        title='托管域名加载失败'
        onRetry={() => void zones.refetch()}
      />
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
            disabled={mutation.isPending}
            placeholder='华东边缘集群'
          />
        )}
      />
      <Controller
        control={form.control}
        name='dns_zone_id'
        render={({ field, fieldState }) => (
          <Choice
            label='托管域名'
            value={field.value}
            onChange={field.onChange}
            items={zoneItems}
            required
            error={fieldState.error?.message}
            hint={zoneHint}
            disabled={mutation.isPending || zones.isPending}
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
            disabled={mutation.isPending}
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
      <Controller
        control={form.control}
        name='status'
        render={({ field }) => (
          <Choice
            label='启用状态'
            value={field.value}
            onChange={field.onChange}
            disabled={mutation.isPending}
            items={[
              { id: 'enabled', label: '已启用' },
              { id: 'disabled', label: '已停用' },
            ]}
          />
        )}
      />
    </FormSheet>
  )
}
