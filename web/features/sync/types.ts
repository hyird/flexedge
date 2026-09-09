import { z } from 'zod'

export const syncEventSchema = z.object({
  sequence: z.number().int(),
  resource_type: z.enum(['provider', 'dns_zone', 'certificate', 'website']),
  resource_id: z.string(),
  operation: z.string(),
  version: z.number().int(),
  outcome: z.enum(['completed', 'failed']),
  emitted_at: z.string(),
})
export const syncEventPageSchema = z.object({
  list: z.array(syncEventSchema),
  cursor: z.number().int(),
  has_more: z.boolean(),
})
export type SyncEvent = z.infer<typeof syncEventSchema>
export type SyncResourceType = SyncEvent['resource_type']
