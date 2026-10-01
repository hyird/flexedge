import { z } from 'zod'
import { Controller, useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import { Choice, Dialog, Field, FormActions, Notice } from '@/components/forms'
import { QueryNotice } from '@/components/page'
import { dnsZoneOptionsQuery } from '@/features/dns-zones/data'
import { saveCluster } from './data'
import type { Cluster } from './types'

const clusterFormSchema = z.object({
  name: z
    .string()
    .trim()
    .min(1, '请输入集群名称')
    .max(100, '集群名称最多 100 个字符'),
  dns_zone_id: z.string().uuid('请选择托管域名'),
  hostname_prefix: z
    .string()
    .trim()
    .min(1, '请输入主机前缀')
    .max(63)
    .regex(
      /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/,
      '仅支持字母、数字和连字符，不能以连字符开头或结尾'
    ),
  status: z.enum(['enabled', 'disabled']),
})
type ClusterFormValues = z.infer<typeof clusterFormSchema>

export function ClusterDialog({
  cluster,
  onClose,
}: {
  cluster?: Cluster
  onClose: () => void
}) {
  const client = useQueryClient()
  const options = useQuery(dnsZoneOptionsQuery)
  const form = useForm<ClusterFormValues>({
    resolver: zodResolver(clusterFormSchema),
    defaultValues: {
      name: cluster?.name ?? '',
      dns_zone_id: cluster?.dns_zone_id ?? '',
      hostname_prefix: cluster?.hostname_prefix ?? '',
      status: cluster?.status === 'disabled' ? 'disabled' : 'enabled',
    },
  })
  const selectedZone = options.data?.find(
    (zone) => zone.id === form.watch('dns_zone_id')
  )
  const prefix = form.watch('hostname_prefix')
  const mutation = useMutation({
    mutationFn: (values: ClusterFormValues) => saveCluster(values, cluster),
    onSuccess: (response) => {
      toast.success(response.message)
      void client.invalidateQueries({ queryKey: queryKeys.clusters })
      onClose()
    },
  })
  const zoneItems = (options.data ?? [])
    .filter((zone) => zone.available || zone.id === cluster?.dns_zone_id)
    .map((zone) => ({
      id: zone.id,
      label: `${zone.domain} · ${zone.dns_provider_name}`,
    }))
  if (cluster && !zoneItems.some((zone) => zone.id === cluster.dns_zone_id))
    zoneItems.push({ id: cluster.dns_zone_id, label: cluster.dns_zone_domain })
  return (
    <Dialog
      title={cluster ? '编辑集群' : '创建集群'}
      onClose={onClose}
      busy={mutation.isPending}
    >
      <form
        className='grid gap-4'
        onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
      >
        <p className='text-sm text-muted'>
          接入域名由主机前缀和托管域名组合生成。
        </p>
        <QueryNotice query={options} onRetry={() => void options.refetch()} />
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
            <div className='grid gap-1'>
              <Choice
                label='托管域名'
                value={field.value}
                onChange={field.onChange}
                items={zoneItems}
                required
                disabled={mutation.isPending || options.isPending}
              />
              {fieldState.error && (
                <p className='text-xs text-danger'>
                  {fieldState.error.message}
                </p>
              )}
            </div>
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
        {prefix && selectedZone && (
          <div className='rounded-lg bg-surface-secondary p-3'>
            <p className='text-xs text-muted'>接入域名</p>
            <code className='mt-1 block text-sm wrap-anywhere'>
              {prefix}.{selectedZone.domain}
            </code>
          </div>
        )}
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
        {mutation.isError && <Notice>{apiErrorMessage(mutation.error)}</Notice>}
        <FormActions
          onCancel={onClose}
          busy={mutation.isPending}
          label={cluster ? '保存集群' : '创建集群'}
        />
      </form>
    </Dialog>
  )
}
