import { z } from 'zod'
import { queryOptions, type QueryClient } from '@tanstack/react-query'
import { getData, pageDataSchema, sendData } from '@/lib/api'
import { parseResponseData } from '@/lib/api-response'
import { readLiveQuery } from '@/lib/live-query'
import {
  securityPolicySchema,
  type SecurityPolicy,
  type SecurityPolicyInput,
} from './transport'

export const managedRuleSchema = z.object({
  id: z.number().int(),
  name: z.string(),
  category: z.string(),
  paranoia_level: z.number().int(),
})
export const securityListMemberSchema = z.object({
  id: z.string().uuid(),
  value: z.string(),
  note: z.string(),
  source: z.string(),
  rule_id: z.string(),
  added_unix_ms: z.number(),
  expires_unix_ms: z.number(),
})
export const securitySharedListSchema = z.object({
  id: z.string().uuid(),
  revision: z.number().int().positive(),
  name: z.string(),
  description: z.string(),
  kind: z.enum(['ip', 'asn', 'country', 'ua']),
  members: z.array(securityListMemberSchema),
})
const securityWebsiteSchema = z.object({
  id: z.string().uuid(),
  name: z.string(),
  policy_id: z.string(),
})
export const securityEventSchema = z.object({
  id: z.string(),
  time: z.string(),
  ip: z.string(),
  country: z.string(),
  method: z.string(),
  path: z.string(),
  action: z.enum(['block', 'challenge', 'log', 'skip']),
  stage: z.enum(['custom', 'rate', 'managed']),
  ruleId: z.string(),
  ruleName: z.string(),
  policyId: z.string(),
  website: z.string(),
  detail: z.string(),
  listName: z.string().optional(),
  operation: z.string().optional(),
})
export const securityTelemetrySchema = z.object({
  requests: z.number().nonnegative(),
  events: z.array(securityEventSchema),
  hourly: z.array(
    z.object({ label: z.string(), block: z.number(), challenge: z.number() })
  ),
  distribution: z.record(z.string(), z.number()),
})
export const securityWorkspaceQuery = queryOptions({
  queryKey: ['security', 'workspace'],
  retry: false,
  meta: { inlineError: true },
  queryFn: async ({ signal }) => {
    const policies = async () => {
      const values: SecurityPolicy[] = []
      for (let page = 1; page <= 10; page++) {
        const result = parseResponseData(
          pageDataSchema(securityPolicySchema),
          await getData<unknown>(
            '/security/policies',
            { page, page_size: 100 },
            signal
          )
        )
        values.push(...result.list)
        if (page >= result.total_pages) return values
      }
      throw new Error('规则集数量超过本页读取上限')
    }
    const [items, lists, websites, catalog] = await Promise.all([
      policies(),
      getData<unknown>('/security/lists', undefined, signal).then((value) =>
        parseResponseData(z.array(securitySharedListSchema), value)
      ),
      getData<unknown>('/security/websites', undefined, signal).then((value) =>
        parseResponseData(z.array(securityWebsiteSchema), value)
      ),
      getData<unknown>('/security/managed-rules', undefined, signal).then(
        (value) => parseResponseData(z.array(managedRuleSchema), value)
      ),
    ])
    return { policies: items, lists, websites, catalog }
  },
})
export const securityTelemetryQuery = queryOptions({
  queryKey: ['security', 'telemetry'],
  retry: false,
  meta: { inlineError: true },
  queryFn: ({ queryKey, signal }) =>
    readLiveQuery('/api/security/stream', queryKey, signal, (value) =>
      parseResponseData(securityTelemetrySchema, value)
    ),
})
export function saveSharedSecurityList(
  input: Omit<z.infer<typeof securitySharedListSchema>, 'id' | 'revision'>,
  current?: { id: string; revision: number }
) {
  return current
    ? sendData('put', '/security/lists/' + current.id, input, current.revision)
    : sendData('post', '/security/lists', input)
}
export function testSecurityRule(
  policy: string,
  rule: string,
  request: Record<string, string>,
  count: number,
  website_id: string
) {
  return sendData<{ matched: boolean; message: string }>(
    'post',
    `/security/policies/${policy}/rules/${rule}/test`,
    { request, count, website_id }
  )
}

const securityPolicyOptionSchema = z.object({
  id: z.string().uuid(),
  name: z.string(),
  description: z.string(),
  website_count: z.number().int().nonnegative(),
})

export function securityPoliciesQuery(params: {
  page: number
  page_size: number
  keyword?: string
}) {
  return queryOptions({
    queryKey: ['security', 'policies', params],
    retry: false,
    meta: { inlineError: true },
    queryFn: async ({ signal }) =>
      parseResponseData(
        pageDataSchema(securityPolicySchema),
        await getData<unknown>('/security/policies', params, signal)
      ),
  })
}
export function securityPolicyQuery(id: string) {
  return queryOptions({
    queryKey: ['security', 'policy', id],
    retry: false,
    meta: { inlineError: true },
    queryFn: async ({ signal }) =>
      parseResponseData(
        securityPolicySchema,
        await getData<unknown>('/security/policies/' + id, undefined, signal)
      ),
  })
}
export function saveSecurityPolicy(
  input: SecurityPolicyInput,
  current?: Pick<SecurityPolicy, 'id' | 'revision'>
) {
  return current
    ? sendData<SecurityPolicy>(
        'put',
        '/security/policies/' + current.id,
        input,
        current.revision
      )
    : sendData<SecurityPolicy>('post', '/security/policies', input)
}
export function removeSecurityPolicy(
  current: Pick<SecurityPolicy, 'id' | 'revision'>
) {
  return sendData(
    'delete',
    '/security/policies/' + current.id,
    undefined,
    current.revision
  )
}
export function afterSecurityPolicyWrite(client: QueryClient) {
  return Promise.all([
    client.invalidateQueries({ queryKey: ['security'] }),
    client.invalidateQueries({ queryKey: ['websites'] }),
  ])
}

export const securityPolicyOptionsQuery = queryOptions({
  queryKey: ['security', 'policy-options'],
  retry: false,
  meta: { inlineError: true },
  queryFn: async ({ signal }) =>
    parseResponseData(
      z.array(securityPolicyOptionSchema),
      await getData<unknown>('/security/policies/options', undefined, signal)
    ),
})
