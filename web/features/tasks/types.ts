import { z } from 'zod'
import { collectionPageSchema } from '@/lib/pagination'

// service/domains/task/task.types.h and task.service.h define the wire contract.
export const taskSchema = z.object({
  id: z.string(),
  resource_type: z.enum([
    'provider',
    'dns_zone',
    'certificate',
    'website',
    'node',
  ]),
  resource_id: z.string(),
  name: z.string(),
  operation: z.string(),
  version: z.number().int().nonnegative(),
  status: z.enum([
    'queued',
    'running',
    'retrying',
    'completed',
    'recovered',
    'failed',
    'superseded',
  ]),
  error: z.string(),
  failures: z.number().int().nonnegative(),
  updated_at: z.string(),
  next_attempt_at: z.string(),
})
export const taskPageSchema = collectionPageSchema.extend({
  list: z.array(taskSchema),
  active: z.number().int().nonnegative(),
  failed: z.number().int().nonnegative(),
})
export const taskHistorySchema = z.object({
  list: z.array(
    z.object({
      outcome: z.enum(['completed', 'failed']),
      error: z.string(),
      emitted_at: z.string(),
    })
  ),
  truncated: z.boolean(),
})
export type Task = z.infer<typeof taskSchema>
export type TaskPage = z.infer<typeof taskPageSchema>
export type TaskHistory = z.infer<typeof taskHistorySchema>
