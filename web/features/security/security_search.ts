import { z } from 'zod'

// The router parses an all-digit rule ID in the query string as a number.
const text = z.preprocess(
  (value) => (typeof value === 'number' ? String(value) : value),
  z.string().max(200).catch('')
)
export const securitySearchSchema = z.object({
  q: text,
  kind: z.enum(['ip', 'asn', 'country', 'ua', '']).catch(''),
  stage: z.enum(['custom', 'rate', 'managed', '']).catch(''),
  action: z.enum(['block', 'challenge', 'log', 'skip', '']).catch(''),
  policy: text,
  event: text,
  page: z.coerce.number().int().min(1).max(1000).catch(1),
})
export type SecuritySearch = z.infer<typeof securitySearchSchema>
