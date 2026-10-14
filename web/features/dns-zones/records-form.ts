import { z } from 'zod'

const recordSchema = z.object({
  id: z.string().uuid(),
  type: z.enum(['A', 'AAAA', 'CNAME', 'TXT', 'MX']),
  name: z
    .string()
    .trim()
    .min(1, '请输入主机记录')
    .max(253, '主机记录最多 253 个字符'),
  content: z
    .string()
    .min(1, '请输入记录值')
    .max(4096, '记录值最多 4096 个字符'),
  ttl: z
    .number('请输入 TTL')
    .int('TTL 必须是整数')
    .min(1, 'TTL 范围为 1–86400 秒')
    .max(86400, 'TTL 范围为 1–86400 秒'),
  priority: z
    .number('请输入优先级')
    .int('优先级必须是整数')
    .min(0, '优先级范围为 0–65535')
    .max(65535, '优先级范围为 0–65535')
    .optional(),
  proxied: z.boolean(),
  line_code: z.string().trim().min(1, '请选择解析线路').max(64),
})

export const recordsSchema = z.object({
  records: z.array(recordSchema).max(10000),
})
