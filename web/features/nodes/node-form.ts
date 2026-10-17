import { z } from 'zod'
import { statusIds } from '@/lib/status'
import type { NodeInput } from './data'
import { nodeStatuses } from './node-display'
import type { Node } from './types'

const ipv4 = z.ipv4(),
  ipv6 = z.ipv6()
function isNodeIpAddress(value: string) {
  // Linux Asio parses the IPv6 address before '%' and keeps the scope separately.
  return (
    ipv4.safeParse(value).success ||
    ipv6.safeParse(value.split('%', 1)[0]).success
  )
}

const endpointSchema = z.object({
  id: z.string().uuid(),
  ip_address: z
    .string()
    .trim()
    .min(1, '请输入IP地址')
    .max(45)
    .refine(isNodeIpAddress, 'IP地址格式不正确'),
  line_code: z.string().trim().min(1, '请输入线路代码').max(64),
})

export const nodeFormSchema = z.object({
  cluster_id: z.string().uuid('请选择所属集群'),
  name: z.string().trim().min(1, '请输入节点名称').max(100),
  status: z.enum(statusIds(nodeStatuses)),
  endpoints: z
    .array(endpointSchema)
    .min(1, '至少配置一个IP')
    .max(8)
    .superRefine((endpoints, ctx) => {
      const ids = new Set<string>(),
        addresses = new Set<string>()
      endpoints.forEach((endpoint, index) => {
        if (ids.has(endpoint.id))
          ctx.addIssue({
            code: 'custom',
            path: [index, 'ip_address'],
            message: 'Endpoint ID不能重复，请重新添加此项',
          })
        if (addresses.has(endpoint.ip_address))
          ctx.addIssue({
            code: 'custom',
            path: [index, 'ip_address'],
            message: 'IP地址不能重复',
          })
        ids.add(endpoint.id)
        addresses.add(endpoint.ip_address)
      })
    }),
})

export type NodeFormValues = z.infer<typeof nodeFormSchema>

/** Form values for a new node (optionally in a preset cluster) or an
 * existing node's saved configuration. */
export function nodeFormValues(
  node?: Pick<Node, 'cluster_id' | 'name' | 'status' | 'config'>,
  clusterId = ''
): NodeFormValues {
  return {
    cluster_id: node?.cluster_id ?? clusterId,
    name: node?.name ?? '',
    status: node?.status ?? 'enabled',
    endpoints: node
      ? node.config.endpoints.map((endpoint) => ({ ...endpoint }))
      : [{ id: crypto.randomUUID(), ip_address: '', line_code: 'default' }],
  }
}

export function nodeInputFromForm(values: NodeFormValues): NodeInput {
  return {
    cluster_id: values.cluster_id,
    name: values.name,
    status: values.status,
    config: { endpoints: values.endpoints },
  }
}
