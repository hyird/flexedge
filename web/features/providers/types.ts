import { z } from 'zod'
import { revisionedSchema } from '@/lib/resource'
import { statusIds } from '@/lib/status'
import { providerStatuses } from './provider-display'

// The wire DTOs of `service/domains/provider/{dns,certificate}_provider_types.h`.
// Platform, credential mode and status are closed sets enforced by the
// `sys_provider` CHECK constraints (`service/config/schema.h`); optional fields
// are `RUVIA_OMIT_EMPTY` and absent when unset, never null.

export const dnsPlatformSchema = z.enum(['cloudflare', 'aliyun'])
export const certificateAuthoritySchema = z.enum(['letsencrypt', 'zerossl'])
export const credentialModeSchema = z.enum(['email', 'access_key'])
const providerStatusSchema = z.enum(statusIds(providerStatuses))

export const dnsProviderSchema = revisionedSchema.extend({
  name: z.string(),
  account_id: z.string(),
  provider: dnsPlatformSchema,
  token_hint: z.string(),
  status: providerStatusSchema,
  zone_count: z.number().int(),
  last_verified_at: z.string().optional(),
  last_error: z.string().optional(),
})
export type DnsProvider = z.infer<typeof dnsProviderSchema>

export const certificateProviderSchema = revisionedSchema.extend({
  provider: certificateAuthoritySchema,
  credential_mode: credentialModeSchema,
  account_email: z.string().optional(),
  access_key_hint: z.string().optional(),
  status: providerStatusSchema,
  last_verified_at: z.string().optional(),
  last_error: z.string().optional(),
})
export type CertificateProvider = z.infer<typeof certificateProviderSchema>

export type DnsProviderInput = {
  name: string
  provider: DnsProvider['provider']
  account_id: string
  api_token: string
}
export type CertificateProviderInput = {
  provider: CertificateProvider['provider']
  credential_mode: CertificateProvider['credential_mode']
  account_email: string
  access_key: string
}
export type ProviderTarget = {
  kind: 'dns' | 'certificate'
  id: string
  revision: number
}
