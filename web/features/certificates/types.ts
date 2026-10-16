import { z } from 'zod'
import { revisionedSchema } from '@/lib/resource'
import { statusIds } from '@/lib/status'
import { certificateStatuses } from './certificate-display'

/** `certificate_dto` (service/domains/certificate/certificate_types.h, filled
 * by certificate_mapper.h). Optional fields are omitted while unset or empty;
 * the server never sends null. */
export const certificateSchema = revisionedSchema.extend({
  domains: z.array(z.string()),
  issuer: z.string(),
  certificate_provider_id: z.string(),
  certificate_provider: z.string(),
  // The database constrains it to these values (`ck_certificate_status`).
  status: z.enum(statusIds(certificateStatuses)),
  usable: z.boolean(),
  config: z.object({ auto_renew: z.boolean() }),
  dns_zone_id: z.string(),
  dns_zone_domain: z.string(),
  not_before: z.string().optional(),
  expires_at: z.string().optional(),
  remaining_days: z.number().int().optional(),
  last_error: z.string().optional(),
  serial_number: z.string().optional(),
  fingerprint_sha256: z.string().optional(),
  last_issued_at: z.string().optional(),
  // A sync task state owned by the tasks domain; unknown values keep their
  // raw text in the chip.
  sync_status: z.string().optional(),
  sync_count_fails: z.number().int().optional(),
  website_count: z.number().int(),
})
export type Certificate = z.infer<typeof certificateSchema>
