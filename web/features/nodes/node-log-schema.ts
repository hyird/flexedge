import { z } from 'zod'
import { createLogEventParser } from '@/features/logs/log-event'

// service/domains/node/node.types.h: NodeLogDto
export const nodeLogSchema = z.object({
  id: z.string(),
  occurred_at: z.string(),
  level: z.string(),
  category: z.string(),
  message: z.string(),
})
export type NodeLog = z.infer<typeof nodeLogSchema>
export const parseNodeLogs = createLogEventParser(nodeLogSchema)
