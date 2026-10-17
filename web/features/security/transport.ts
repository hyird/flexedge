import { z } from 'zod'

const ruleIdentity = {
  id: z.string().trim().min(1, '请输入规则名称').max(100),
  enabled: z.boolean(),
  expression: z.string().trim().min(1, '请输入匹配表达式').max(8000),
  name: z.string().max(256).optional(),
  expression_mode: z.boolean().optional(),
}
export const customRuleSchema = z
  .object({
    ...ruleIdentity,
    action: z.enum(['block', 'log', 'skip']),
    skip_stages: z.array(z.enum(['custom', 'rate', 'managed'])).optional(),
    response_status: z.number().int().min(400).max(499).optional(),
    response_content_type: z
      .enum([
        'text/plain',
        'text/html',
        'application/json',
        'application/xml',
        '',
      ])
      .optional(),
    response_body: z
      .string()
      .refine(
        (text) => new TextEncoder().encode(text).length <= 2048,
        '响应内容不能超过 2KiB'
      )
      .optional(),
  })
  .superRefine((value, context) => {
    if (
      value.response_content_type === 'application/json' &&
      value.response_body
    ) {
      try {
        JSON.parse(value.response_body)
      } catch {
        context.addIssue({
          code: 'custom',
          path: ['response_body'],
          message: '请输入有效的 JSON',
        })
      }
    }
  })
export const ccRuleSchema = z.object({
  ...ruleIdentity,
  action: z.enum(['block', 'log']),
  requests: z.number().int().min(1).max(1_000_000_000),
  window_seconds: z.number().int().min(1).max(3600),
  mitigation_seconds: z.number().int().min(1).max(86400),
  response_status: z.number().int().min(400).max(499).optional(),
  response_content_type: z
    .enum([
      'text/plain',
      'text/html',
      'application/json',
      'application/xml',
      '',
    ])
    .optional(),
  response_body: z.string().optional(),
})
export const securityListSchema = z.object({
  name: z
    .string()
    .min(1, '请输入清单名称')
    .max(100)
    .regex(/^[a-z0-9_]+$/, '使用小写字母、数字和下划线'),
  type: z.enum(['ip', 'asn', 'country', 'user_agent']),
  entries: z.array(z.string().min(1).max(2048)).max(100000),
})
export const securityConfigSchema = z
  .object({
    custom_rules: z.array(customRuleSchema).max(100),
    cc_rules: z.array(ccRuleSchema).max(100),
    managed: z.object({
      enabled: z.boolean(),
      paranoia_level: z.number().int().min(1).max(4),
      inbound_threshold: z.number().int().min(1).max(1000),
      body_limit_bytes: z
        .number()
        .int()
        .min(1024)
        .max(8 * 1024 ** 2),
      detection_only: z.boolean(),
      excluded_rules: z
        .array(z.number().int().min(910000).max(949999))
        .max(512),
    }),
    lists: z.array(securityListSchema).max(100),
  })
  .superRefine((config, context) => {
    const identities = new Set<string>()
    for (const kind of ['custom_rules', 'cc_rules'] as const) {
      config[kind].forEach((rule, index) => {
        if (identities.has(rule.id))
          context.addIssue({
            code: 'custom',
            path: [kind, index, 'id'],
            message: '规则名称不能重复',
          })
        identities.add(rule.id)
      })
    }
    const names = new Set<string>()
    let total = 0
    config.lists.forEach((list, index) => {
      if (names.has(list.name))
        context.addIssue({
          code: 'custom',
          path: ['lists', index, 'name'],
          message: '清单名称不能重复',
        })
      names.add(list.name)
      total += list.entries.length
    })
    if (total > 100000)
      context.addIssue({
        code: 'custom',
        path: ['lists'],
        message: '清单条目总数不能超过 100000',
      })
  })
export const securityPolicyInputSchema = z.object({
  name: z.string().trim().min(1, '请输入规则集名称').max(100),
  description: z.string().max(1000),
  config: securityConfigSchema,
})
export const securityPolicySchema = securityPolicyInputSchema.extend({
  id: z.string().uuid(),
  revision: z.number().int().positive(),
  website_count: z.number().int().nonnegative(),
  created_at: z.string(),
  updated_at: z.string(),
})
export type SecurityPolicy = z.infer<typeof securityPolicySchema>
export type SecurityPolicyInput = z.infer<typeof securityPolicyInputSchema>
export type SecurityConfig = z.infer<typeof securityConfigSchema>
export type CustomRule = z.infer<typeof customRuleSchema>
export type CcRule = z.infer<typeof ccRuleSchema>
export type SecurityList = z.infer<typeof securityListSchema>
export function defaultSecurityPolicy(): SecurityPolicyInput {
  return {
    name: '',
    description: '',
    config: {
      custom_rules: [],
      cc_rules: [],
      lists: [],
      managed: {
        enabled: true,
        paranoia_level: 1,
        inbound_threshold: 5,
        body_limit_bytes: 1024 ** 2,
        detection_only: false,
        excluded_rules: [],
      },
    },
  }
}
