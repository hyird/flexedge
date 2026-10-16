import { z } from 'zod'
import { enablementStatuses, statusIds } from '@/lib/status'
import type { ClusterInput } from './data'
import type { Cluster } from './types'

export const clusterFormSchema = z.object({
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
  status: z.enum(statusIds(enablementStatuses)),
})
export type ClusterFormValues = z.infer<typeof clusterFormSchema>

export function clusterFormValues(cluster?: Cluster): ClusterInput {
  return {
    name: cluster?.name ?? '',
    dns_zone_id: cluster?.dns_zone_id ?? '',
    hostname_prefix: cluster?.hostname_prefix ?? '',
    status: cluster?.status ?? 'enabled',
  }
}
