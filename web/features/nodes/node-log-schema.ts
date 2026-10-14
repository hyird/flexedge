import { z } from 'zod'
import { createLogEventParser } from '@/features/logs/log-event'

// service/domains/node/node_types.h: NodeLogDto
const nodeLogSchema = z.object({
  id: z.string(),
  occurred_at: z.string(),
  level: z.string(),
  category: z.string(),
  message: z.string(),
})
export const parseNodeLogs = createLogEventParser(nodeLogSchema)
