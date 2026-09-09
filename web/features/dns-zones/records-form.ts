import { z } from 'zod'

const recordSchema = z.object({
  id: z.string().uuid(),
  type: z.enum(['A', 'AAAA', 'CNAME', 'TXT', 'MX']),
  name: z.string().trim().min(1, '请输入主机记录').max(253),
  content: z.string().min(1, '请输入记录值').max(4096),
  ttl: z.number().int().min(1).max(86400),
  priority: z.number().int().min(0).max(65535).optional(),
  proxied: z.boolean(),
  line_code: z.string().trim().min(1, '请输入线路代码').max(64),
})

export const recordsSchema = z.object({
  records: z.array(recordSchema).max(10000),
})
export type RecordsValues = z.infer<typeof recordsSchema>
