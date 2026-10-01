import {
  Controller,
  useFieldArray,
  useForm,
  type Resolver,
} from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { Accordion, Button, Tabs, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { formatBytes, formatDate } from '@/lib/format'
import { queryKeys } from '@/lib/query-keys'
import { Drawer } from '@/components/drawer'
import { Choice, Field, FormActions, Notice } from '@/components/forms'
import { QueryNotice } from '@/components/page'
import { StatusChip } from '@/components/status-chip'
import type { Cluster } from '@/features/clusters/types'
import { dnsZoneLinesQuery } from '@/features/dns-zones/data'
import { dnsLinePath } from '@/features/dns-zones/dns-lines'
import { createNode, updateNode, type NodeCredentials } from './data'
import { nodeFormSchema, type NodeFormValues } from './node-form'
import type { Node } from './types'

export function NodeDialog({
  node,
  clusters,
  initialClusterId,
  onClose,
  onCredentials,
}: {
  node?: Node
  clusters: Cluster[]
  initialClusterId?: string
  onClose: () => void
  onCredentials: (credentials: NodeCredentials) => void
}) {
  const client = useQueryClient()
  const form = useForm<NodeFormValues>({
    resolver: zodResolver(nodeFormSchema) as Resolver<NodeFormValues>,
    defaultValues: {
      cluster_id: node?.cluster_id ?? initialClusterId ?? '',
      name: node?.name ?? '',
      status: node?.status === 'disabled' ? 'disabled' : 'enabled',
      endpoints: node?.config.endpoints ?? [
        { id: crypto.randomUUID(), ip_address: '', line_code: 'default' },
      ],
      cache: node?.config.cache ?? {
        memory_bytes: 67108864,
        disk_bytes: 1073741824,
        max_object_bytes: 268435456,
        directory: 'cache',
      },
    },
  })
  const clusterId = form.watch('cluster_id')
  const selectedCluster = clusters.find((cluster) => cluster.id === clusterId)
  const lines = useQuery(dnsZoneLinesQuery(selectedCluster?.dns_zone_id))
  const endpoints = useFieldArray({
    control: form.control,
    name: 'endpoints',
    keyName: 'formKey',
  })
  const mutation = useMutation<
    { code: number; message: string; data: NodeCredentials | undefined },
    Error,
    NodeFormValues
  >({
    mutationFn: (values: NodeFormValues) => {
      const input = {
        cluster_id: values.cluster_id,
        name: values.name,
        status: values.status,
        config: { endpoints: values.endpoints, cache: values.cache },
      }
      return node ? updateNode(node, input) : createNode(input)
    },
    onSuccess: (response) => {
      toast.success(response.message)
      void client.invalidateQueries({ queryKey: queryKeys.nodes })
      void client.invalidateQueries({ queryKey: queryKeys.clusters })
      onClose()
      if (response.data) onCredentials(response.data)
    },
  })
  const fieldErrors = form.formState.errors
  const endpointError =
    fieldErrors.endpoints?.root?.message ?? fieldErrors.endpoints?.message
  return (
    <Drawer
      title={node ? '编辑节点' : '添加节点'}
      onClose={onClose}
      busy={mutation.isPending}
      size='lg'
    >
      <Tabs defaultSelectedKey='configuration' variant='secondary'>
        <Tabs.ListContainer>
          <Tabs.List aria-label='节点详情'>
            <Tabs.Tab id='configuration'>
              节点配置
              <Tabs.Indicator />
            </Tabs.Tab>
            {node && (
              <Tabs.Tab id='runtime'>
                运行状态
                <Tabs.Indicator />
              </Tabs.Tab>
            )}
          </Tabs.List>
        </Tabs.ListContainer>
        <Tabs.Panel id='configuration' className='pt-4'>
          <form
            className='grid gap-5'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <div className='grid gap-3 sm:grid-cols-2'>
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
                    disabled={mutation.isPending}
                  />
                )}
              />
              <Controller
                control={form.control}
                name='cluster_id'
                render={({ field, fieldState }) => (
                  <div className='grid gap-1'>
                    <Choice
                      label='所属集群'
                      value={field.value}
                      onChange={field.onChange}
                      items={clusters.map((cluster) => ({
                        id: cluster.id,
                        label: cluster.name,
                      }))}
                      disabled={mutation.isPending}
                      required
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
            </div>
            <section className='grid gap-3' aria-label='服务端点'>
              <div className='flex items-center justify-between gap-3'>
                <div>
                  <h3 className='text-sm font-medium'>服务端点</h3>
                  <p className='mt-1 text-xs text-muted'>
                    最多配置 8 个唯一 IP，为每个 IP 指定 DNS 线路。
                  </p>
                </div>
                <Button
                  size='sm'
                  variant='secondary'
                  isDisabled={
                    endpoints.fields.length >= 8 || mutation.isPending
                  }
                  onPress={() =>
                    endpoints.append({
                      id: crypto.randomUUID(),
                      ip_address: '',
                      line_code: 'default',
                    })
                  }
                >
                  添加 IP
                </Button>
              </div>
              <QueryNotice query={lines} onRetry={() => void lines.refetch()} />
              {endpointError && <Notice>{endpointError}</Notice>}
              {endpoints.fields.map((endpoint, index) => {
                const currentCode = form.watch(`endpoints.${index}.line_code`)
                const available = (lines.data ?? [])
                  .filter(
                    (line) =>
                      line.status !== 'disabled' || line.code === currentCode
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
                    className='grid items-start gap-2 rounded-lg bg-surface-secondary p-3 sm:grid-cols-[minmax(0,1fr)_minmax(0,1fr)_auto]'
                  >
                    <Controller
                      control={form.control}
                      name={`endpoints.${index}.ip_address`}
                      render={({ field, fieldState }) => (
                        <Field
                          label={`IP 地址 ${index + 1}`}
                          value={field.value}
                          onChange={field.onChange}
                          error={fieldState.error?.message}
                          placeholder='203.0.113.10 或 IPv6'
                          required
                          disabled={mutation.isPending}
                        />
                      )}
                    />
                    <Controller
                      control={form.control}
                      name={`endpoints.${index}.line_code`}
                      render={({ field, fieldState }) => (
                        <div className='grid gap-1'>
                          <Choice
                            label='DNS 线路'
                            value={field.value}
                            onChange={field.onChange}
                            items={available}
                            disabled={mutation.isPending || lines.isPending}
                          />
                          {fieldState.error && (
                            <p className='text-xs text-danger'>
                              {fieldState.error.message}
                            </p>
                          )}
                        </div>
                      )}
                    />
                    <Button
                      size='sm'
                      variant='tertiary'
                      className='sm:mt-6'
                      isDisabled={
                        endpoints.fields.length === 1 || mutation.isPending
                      }
                      onPress={() => endpoints.remove(index)}
                    >
                      移除
                    </Button>
                  </div>
                )
              })}
            </section>
            <Accordion>
              <Accordion.Item id='cache'>
                <Accordion.Heading>
                  <Accordion.Trigger>
                    缓存资源
                    <Accordion.Indicator />
                  </Accordion.Trigger>
                </Accordion.Heading>
                <Accordion.Panel>
                  <Accordion.Body>
                    <p className='mb-3 text-xs text-muted'>
                      内存和磁盘容量为惰性上限；设为 0 可关闭对应缓存层。
                    </p>
                    <div className='grid gap-3 sm:grid-cols-2'>
                      {(
                        [
                          ['memory_bytes', '内存容量（MiB）', 1048576],
                          ['disk_bytes', '磁盘容量（GiB）', 1073741824],
                          ['max_object_bytes', '单对象上限（MiB）', 1048576],
                        ] as const
                      ).map(([name, label, unit]) => (
                        <Controller
                          key={name}
                          control={form.control}
                          name={`cache.${name}`}
                          render={({ field, fieldState }) => (
                            <Field
                              label={label}
                              type='number'
                              value={
                                Number.isFinite(field.value)
                                  ? String(field.value / unit)
                                  : ''
                              }
                              onChange={(value) =>
                                field.onChange(
                                  value === ''
                                    ? Number.NaN
                                    : Math.round(Number(value) * unit)
                                )
                              }
                              min={name === 'max_object_bytes' ? 1 / unit : 0}
                              step='any'
                              required
                              error={fieldState.error?.message}
                              disabled={mutation.isPending}
                            />
                          )}
                        />
                      ))}
                      <Controller
                        control={form.control}
                        name='cache.directory'
                        render={({ field, fieldState }) => (
                          <Field
                            label='缓存目录'
                            value={field.value}
                            onChange={field.onChange}
                            error={fieldState.error?.message}
                            hint='节点 state 下的相对路径，例如 cache'
                            required
                            disabled={mutation.isPending}
                          />
                        )}
                      />
                    </div>
                  </Accordion.Body>
                </Accordion.Panel>
              </Accordion.Item>
            </Accordion>
            {mutation.isError && (
              <Notice>{apiErrorMessage(mutation.error)}</Notice>
            )}
            {Object.keys(fieldErrors).length > 0 && (
              <Notice>
                请检查节点配置中的错误字段，缓存资源可展开后修改。
              </Notice>
            )}
            <FormActions
              onCancel={onClose}
              busy={mutation.isPending}
              label={node ? '保存配置' : '创建节点'}
            />
          </form>
        </Tabs.Panel>
        {node && (
          <Tabs.Panel id='runtime' className='pt-4'>
            <NodeRuntime node={node} />
          </Tabs.Panel>
        )}
      </Tabs>
    </Drawer>
  )
}

function NodeRuntime({ node }: { node: Node }) {
  const stats = node.runtime.cache_statistics
  const runtime = node.runtime
  return (
    <div className='grid gap-5'>
      <div className='flex flex-wrap gap-2'>
        <StatusChip status={runtime.connection_status} />
        <StatusChip status={runtime.registration_status} />
      </div>
      <dl className='grid grid-cols-2 gap-x-5 gap-y-3 text-sm'>
        {(
          [
            ['Agent 版本', runtime.agent_version ?? '—'],
            ['最近心跳', formatDate(runtime.last_heartbeat_at)],
            ['CPU', `${runtime.cpu_usage?.toFixed(1) ?? '—'}%`],
            ['内存', `${runtime.memory_usage?.toFixed(1) ?? '—'}%`],
            [
              '配置修订',
              `${runtime.applied_node_spec_revision} / ${node.node_spec_revision}`,
            ],
            ['活动发布', runtime.active_release_id ?? '—'],
            ['连接数', runtime.connection_count?.toLocaleString() ?? '—'],
            ['系统负载', runtime.load_1m?.toFixed(2) ?? '—'],
            ['排队日志', runtime.queued_log_events?.toLocaleString() ?? '—'],
            ['丢弃日志', runtime.dropped_log_events?.toLocaleString() ?? '—'],
          ] as const
        ).map(([label, value]) => (
          <div key={label}>
            <dt className='text-xs text-muted'>{label}</dt>
            <dd className='mt-1 wrap-anywhere tabular-nums'>{value}</dd>
          </div>
        ))}
      </dl>
      {runtime.last_error && <Notice>{runtime.last_error}</Notice>}
      <section className='grid gap-3'>
        <h3 className='text-sm font-medium'>缓存运行统计</h3>
        {stats ? (
          <dl className='grid grid-cols-2 gap-3 sm:grid-cols-3'>
            {(
              [
                ['实际内存', formatBytes(stats.memory_bytes)],
                ['实际磁盘', formatBytes(stats.disk_bytes)],
                ['回源流量', formatBytes(stats.origin_bytes)],
                ['缓存流量', formatBytes(stats.cache_bytes)],
                ['内存命中', stats.memory_hits],
                ['磁盘命中', stats.disk_hits],
                ['未命中', stats.misses],
                ['绕过', stats.bypasses],
                ['陈旧命中', stats.stale_hits],
                ['重新验证', stats.revalidations],
                ['合并回源', stats.coalesced],
                ['等待请求', stats.active_waiters],
                ['缓存对象', stats.entries],
                ['写入', stats.writes],
                ['写入错误', stats.write_errors],
                ['淘汰', stats.evictions],
              ] as const
            ).map(([label, value]) => (
              <div key={label} className='rounded-lg bg-surface-secondary p-3'>
                <dt className='text-xs text-muted'>{label}</dt>
                <dd className='mt-1 text-sm font-medium tabular-nums'>
                  {typeof value === 'number' ? value.toLocaleString() : value}
                </dd>
              </div>
            ))}
          </dl>
        ) : (
          <p className='text-sm text-muted'>缓存统计尚未上报。</p>
        )}
      </section>
    </div>
  )
}
