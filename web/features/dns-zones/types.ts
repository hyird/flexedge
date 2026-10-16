import { z } from 'zod'
import { revisionedSchema } from '@/lib/resource'

// Mirrors service/domains/dns_zone/dns_zone_types.h and the record config of
// service/features/dns_sync/transport.h. `RUVIA_OMIT_EMPTY` fields are absent
// (never null) when empty, so they are `.optional()`.

/** Record types the backend accepts (dns_sync/transport.h rejects others). */
export const dnsRecordTypeSchema = z.enum(['A', 'AAAA', 'CNAME', 'TXT', 'MX'])

/** `dns_line_runtime_dto` */
export const dnsLineSchema = z.object({
  code: z.string(),
  name: z.string(),
  display_name: z.string(),
  status: z.string(),
})
export type DnsLine = z.infer<typeof dnsLineSchema>

/** `zone_record_output` / `projected_dns_record_dto` */
export const dnsRecordSchema = z.object({
  id: z.string(),
  type: dnsRecordTypeSchema,
  name: z.string(),
  content: z.string(),
  ttl: z.number().int(),
  priority: z.number().int().optional(),
  proxied: z.boolean(),
  line_code: z.string(),
})
export type DnsRecord = z.infer<typeof dnsRecordSchema>

/** `dns_record_runtime_dto`; `sync_status` stays a string so a new backend
 * state shows its raw text instead of failing the page. */
export const dnsRecordStateSchema = z.object({
  id: z.string(),
  sync_status: z.string(),
  synced_revision: z.number().int(),
  last_error: z.string().optional(),
})

/** `dns_zone_dto` */
export const dnsZoneSchema = revisionedSchema.extend({
  dns_provider_id: z.string(),
  dns_provider: z.string(),
  dns_provider_name: z.string(),
  domain: z.string(),
  sync_status: z.string(),
  desired_revision: z.number().int(),
  synced_revision: z.number().int(),
  website_count: z.number().int(),
  config: z.object({ records: z.array(dnsRecordSchema) }),
  runtime: z.object({
    lines: z.array(dnsLineSchema),
    projected_records: z.array(dnsRecordSchema),
    record_states: z.array(dnsRecordStateSchema),
  }),
  last_synced_at: z.string().optional(),
  last_error: z.string().optional(),
})
export type DnsZone = z.infer<typeof dnsZoneSchema>

/** `dns_zone_option_dto`; `/dns-zones/collection` returns a bare array
 * (`dns_zone_collection_response`). */
export const dnsZoneOptionSchema = z.object({
  id: z.string(),
  domain: z.string(),
  dns_provider: z.string(),
  dns_provider_name: z.string(),
  sync_status: z.string(),
  available: z.boolean(),
})
export type DnsZoneOption = z.infer<typeof dnsZoneOptionSchema>
export const dnsZoneOptionsSchema = z.array(dnsZoneOptionSchema)

/** `available_dns_zone_list_data_dto` of `/dns-zones/available`. */
export const availableDnsZonesSchema = z.object({
  list: z.array(z.object({ domain: z.string(), status: z.string() })),
})
