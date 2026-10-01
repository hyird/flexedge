import { z } from 'zod'

const ipv4 = z.ipv4(),
  ipv6 = z.ipv6()
const maxMemoryBytes = 1099511627776
const maxDiskBytes = 1125899906842624
const maxObjectBytes = 1099511627776
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
    .min(1, '请输入 IP 地址')
    .max(45)
    .refine(isNodeIpAddress, 'IP 地址格式不正确'),
  line_code: z.string().trim().min(1, '请输入线路代码').max(64),
})

export const nodeFormSchema = z.object({
  cluster_id: z.string().uuid('请选择所属集群'),
  name: z.string().trim().min(1, '请输入节点名称').max(100),
  status: z.enum(['enabled', 'disabled']),
  endpoints: z
    .array(endpointSchema)
    .min(1, '至少配置一个 IP')
    .max(8)
    .superRefine((endpoints, ctx) => {
      const ids = new Set<string>(),
        addresses = new Set<string>()
      endpoints.forEach((endpoint, index) => {
        if (ids.has(endpoint.id))
          ctx.addIssue({
            code: 'custom',
            path: [index, 'ip_address'],
            message: 'Endpoint ID 不能重复，请重新添加此项',
          })
        if (addresses.has(endpoint.ip_address))
          ctx.addIssue({
            code: 'custom',
            path: [index, 'ip_address'],
            message: 'IP 地址不能重复',
          })
        ids.add(endpoint.id)
        addresses.add(endpoint.ip_address)
      })
    }),
  cache: z
    .object({
      memory_bytes: z.number().int().min(0).max(maxMemoryBytes),
      disk_bytes: z.number().int().min(0).max(maxDiskBytes),
      max_object_bytes: z.number().int().min(1).max(maxObjectBytes),
      directory: z
        .string()
        .min(1)
        .max(255)
        .refine((value) => {
          if (
            value.includes('\\') ||
            value.includes(':') ||
            value.endsWith('/')
          )
            return false
          if (value.startsWith('/')) return false
          if (
            [...value].some(
              (char) => char.charCodeAt(0) < 32 || char.charCodeAt(0) === 127
            )
          )
            return false
          return value
            .split('/')
            .every(
              (part) =>
                part !== '' &&
                part !== '.' &&
                part !== '..' &&
                part === part.trim()
            )
        }, '目录必须是节点 state 下的相对路径，且不能包含空、点或父级组件'),
    })
    .default({
      memory_bytes: 67108864,
      disk_bytes: 1073741824,
      max_object_bytes: 268435456,
      directory: 'cache',
    }),
})

export type NodeFormValues = z.infer<typeof nodeFormSchema>
