import { type ReactNode } from 'react'
import {
  Controller,
  useFieldArray,
  useWatch,
  type UseFormReturn,
} from 'react-hook-form'
import { useQuery } from '@tanstack/react-query'
import { Plus, TrashBin } from '@gravity-ui/icons'
import { Button, Tooltip } from '@heroui/react'
import { Choice, Field, Notice } from '@/components/forms'
import { QueryNotice } from '@/components/page'
import { Section } from '@/components/section'
import { clusterOptionsQuery } from '@/features/clusters/data'
import { dnsZoneLinesQuery } from '@/features/dns-zones/data'
import { dnsLinePath } from '@/features/dns-zones/dns-lines'
import type { NodeFormValues } from './node-form'

const maxEndpoints = 8

/** Groups render as cards on the node config page and as plain blocks
 * inside the add-node sheet. */
function Group({
  variant,
  title,
  description,
  actions,
  children,
}: {
  variant: 'page' | 'sheet'
  title: string
  description?: string
  actions?: ReactNode
  children: ReactNode
}) {
  if (variant === 'page')
    return (
      <Section title={title} description={description} actions={actions}>
        {children}
      </Section>
    )
  return (
    <section aria-label={title} className='flex flex-col gap-3'>
      <div className='flex flex-wrap items-start justify-between gap-3'>
        <div className='flex flex-col gap-0.5'>
          <div className='flex min-h-8 items-center'>
            <h3 className='subsection-title'>{title}</h3>
          </div>
          {description && (
            <p className='text-[13px] text-muted'>{description}</p>
          )}
        </div>
        {actions}
      </div>
      {children}
    </section>
  )
}

/** Node name, cluster, status and service endpoints (IP + DNS line of the
 * cluster's zone), shared by creation and editing. */
export function NodeConfigFields({
  form,
  variant,
  disabled,
  lockCluster = false,
}: {
  form: UseFormReturn<NodeFormValues>
  variant: 'page' | 'sheet'
  disabled: boolean
  /** Hide the cluster choice when the cluster is fixed by the context. */
  lockCluster?: boolean
}) {
  const clusters = useQuery(clusterOptionsQuery)
  const clusterId = useWatch({ control: form.control, name: 'cluster_id' })
  const lineCodes = useWatch({ control: form.control, name: 'endpoints' })
  const selectedCluster = clusters.data?.find(
    (cluster) => cluster.id === clusterId
  )
  const lines = useQuery(dnsZoneLinesQuery(selectedCluster?.dns_zone_id))
  const endpoints = useFieldArray({
    control: form.control,
    name: 'endpoints',
    keyName: 'formKey',
  })
  const errors = form.formState.errors
  const endpointError =
    errors.endpoints?.root?.message ?? errors.endpoints?.message
  const basics = (
    <div className='grid gap-4 sm:grid-cols-2'>
      <Controller
        control={form.control}
        name='name'
        render={({ field, fieldState }) => (
          <Field
            label='节点名称'
            value={field.value}
            onChange={field.onChange}
            error={fieldState.error?.message}
            required
            placeholder='edge-tpe-01'
            hint='同一集群内唯一'
            disabled={disabled}
          />
        )}
      />
      {!lockCluster && (
        <Controller
          control={form.control}
          name='cluster_id'
          render={({ field, fieldState }) => (
            <Choice
              label='所属集群'
              value={field.value}
              onChange={field.onChange}
              items={(clusters.data ?? []).map((cluster) => ({
                id: cluster.id,
                label: cluster.name,
              }))}
              error={fieldState.error?.message}
              hint='DNS线路来自集群的托管域名'
              disabled={disabled || clusters.isPending}
              required
            />
          )}
        />
      )}
      <Controller
        control={form.control}
        name='status'
        render={({ field }) => (
          <Choice
            label='启用状态'
            value={field.value}
            onChange={field.onChange}
            disabled={disabled}
            hint='停用的节点不再接收配置分发'
            items={[
              { id: 'enabled', label: '已启用' },
              { id: 'disabled', label: '已停用' },
            ]}
          />
        )}
      />
    </div>
  )
  const endpointRows = (
    <div className='flex flex-col gap-3'>
      <QueryNotice
        query={clusters}
        title='集群列表加载失败'
        onRetry={() => void clusters.refetch()}
      />
      <QueryNotice
        query={lines}
        title='DNS线路加载失败'
        onRetry={() => void lines.refetch()}
      />
      {endpointError && <Notice>{endpointError}</Notice>}
      {endpoints.fields.map((endpoint, index) => {
        const currentCode = lineCodes[index]?.line_code ?? endpoint.line_code
        const available = (lines.data ?? [])
          .filter(
            (line) => line.status !== 'disabled' || line.code === currentCode
          )
          .map((line) => ({
            id: line.code,
            label: dnsLinePath(lines.data ?? [], line.code),
          }))
        if (!available.some((line) => line.id === currentCode))
          available.push({
            id: currentCode,
            label: currentCode === 'default' ? '默认线路' : currentCode,
          })
        return (
          <div
            key={endpoint.formKey}
            className='grid items-start gap-3 rounded-lg border border-border p-3 sm:grid-cols-[minmax(0,1fr)_minmax(0,1fr)_auto]'
          >
            <Controller
              control={form.control}
              name={`endpoints.${index}.ip_address`}
              render={({ field, fieldState }) => (
                <Field
                  label={`IP地址 ${index + 1}`}
                  value={field.value}
                  onChange={field.onChange}
                  error={fieldState.error?.message}
                  placeholder='203.0.113.10 或IPv6'
                  required
                  disabled={disabled}
                />
              )}
            />
            <Controller
              control={form.control}
              name={`endpoints.${index}.line_code`}
              render={({ field, fieldState }) => (
                <Choice
                  label='DNS线路'
                  value={field.value}
                  onChange={field.onChange}
                  items={available}
                  error={fieldState.error?.message}
                  disabled={disabled || (lines.isFetching && !lines.data)}
                />
              )}
            />
            <Tooltip delay={300}>
              <Button
                size='sm'
                variant='ghost'
                isIconOnly
                aria-label={`移除IP地址 ${index + 1}`}
                className='sm:mt-6'
                isDisabled={endpoints.fields.length === 1 || disabled}
                onPress={() => endpoints.remove(index)}
              >
                <TrashBin className='size-4' />
              </Button>
              <Tooltip.Content>移除</Tooltip.Content>
            </Tooltip>
          </div>
        )
      })}
    </div>
  )
  const addEndpoint = (
    <Button
      size='sm'
      variant='tertiary'
      isDisabled={endpoints.fields.length >= maxEndpoints || disabled}
      onPress={() =>
        endpoints.append({
          id: crypto.randomUUID(),
          ip_address: '',
          line_code: 'default',
        })
      }
    >
      <Plus className='size-4' />
      添加IP
    </Button>
  )
  return (
    <>
      <Group variant={variant} title='基本信息'>
        {basics}
      </Group>
      <Group
        variant={variant}
        title='服务端点'
        description={`最多 ${maxEndpoints} 个唯一IP，每个IP指定一条DNS线路。`}
        actions={addEndpoint}
      >
        {endpointRows}
      </Group>
    </>
  )
}
