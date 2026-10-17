import { z } from 'zod'
import type { StatusDictionary } from '@/lib/status'
import {
  builderExpression,
  conditionSchema,
  parseExpression,
} from './expression_model'

export { fields, operators, type Condition } from './expression_model'

export const stages = [
  {
    id: 'custom',
    label: '自定义规则',
    description: '按业务条件阻断、记录或跳过指定防护。',
  },
  {
    id: 'rate',
    label: 'CC / 限速规则',
    description: '按客户端 IP 统计匹配请求，超过阈值后处置。',
  },
  {
    id: 'managed',
    label: '托管 WAF 规则',
    description: '使用预定义规则识别常见 Web 攻击。',
  },
] as const
export type Stage = (typeof stages)[number]['id']
export const actions = {
  block: { label: '阻断', tone: 'danger' },
  challenge: { label: '挑战验证', tone: 'warning' },
  log: { label: '仅记录', tone: 'accent' },
  skip: { label: '跳过', tone: 'default' },
} as const satisfies StatusDictionary
export const ruleSchema = z
  .object({
    id: z.string(),
    name: z.string().trim().min(1, '请填写规则名称').max(512),
    stage: z.enum(['custom', 'rate', 'managed']),
    enabled: z.boolean(),
    action: z.enum(['block', 'challenge', 'log', 'skip']),
    logic: z.enum(['and', 'or']),
    conditions: z.array(conditionSchema).min(1).max(8),
    expression_mode: z.boolean(),
    raw_expression: z.string().max(8000),
    enrollment: z.object({
      enabled: z.boolean(),
      list_id: z.string(),
      ttl_seconds: z
        .number()
        .int()
        .min(60, '保留时间至少为 60 秒')
        .max(2592000, '保留时间最多 30 天'),
      renew: z.boolean(),
    }),
    response: z.object({
      enabled: z.boolean(),
      status: z.number().int().min(400).max(499),
      content_type: z.enum([
        'text/html',
        'text/plain',
        'application/json',
        'application/xml',
      ]),
      body: z.string(),
    }),
    skip: z.array(z.enum(['custom', 'rate', 'managed'])),
    limit: z.number().int('阈值必须为整数').min(1, '阈值至少为 1').max(1000000),
    window: z.number().int().min(1, '窗口至少为 1 秒').max(3600),
    duration: z.number().int().min(1, '处置时长至少为 1 秒').max(86400),
    hits: z.number(),
    signature: z.string(),
  })
  .superRefine((rule, context) => {
    if (rule.stage !== 'managed' && rule.name.length > 80)
      context.addIssue({
        code: 'custom',
        path: ['name'],
        message: '规则名称最多80个字符',
      })
    if (rule.action === 'block' && rule.response.enabled) {
      if (new TextEncoder().encode(rule.response.body).length > 2048)
        context.addIssue({
          code: 'custom',
          path: ['response'],
          message: '响应正文最多 2 KiB',
        })
      if (!rule.response.body.trim())
        context.addIssue({
          code: 'custom',
          path: ['response'],
          message: '请填写响应正文',
        })
      if (rule.response.content_type === 'application/json') {
        try {
          JSON.parse(rule.response.body)
        } catch {
          context.addIssue({
            code: 'custom',
            path: ['response'],
            message: '响应正文不是有效的 JSON',
          })
        }
      }
    }
    if (rule.stage !== 'managed') {
      try {
        parseExpression(
          rule.expression_mode
            ? rule.raw_expression
            : builderExpression(rule.conditions, rule.logic)
        )
      } catch (error) {
        context.addIssue({
          code: 'custom',
          path: ['conditions'],
          message: error instanceof Error ? error.message : '匹配条件无效',
        })
      }
    }
    if (rule.enrollment.enabled && !rule.enrollment.list_id)
      context.addIssue({
        code: 'custom',
        path: ['enrollment'],
        message: '请选择目标清单',
      })
    if (rule.enrollment.enabled && rule.action === 'skip')
      context.addIssue({
        code: 'custom',
        path: ['enrollment'],
        message: '跳过规则不支持自动入列',
      })
    if (rule.action === 'skip' && rule.stage !== 'custom')
      context.addIssue({
        code: 'custom',
        path: ['action'],
        message: '只有自定义规则可以设置跳过动作',
      })
    if (rule.action === 'skip' && rule.skip.length === 0)
      context.addIssue({
        code: 'custom',
        path: ['skip'],
        message: '请至少选择一个跳过范围',
      })
  })
export type Rule = z.infer<typeof ruleSchema>

export function newRule(stage: Stage = 'custom'): Rule {
  return {
    id: crypto.randomUUID(),
    name: '',
    stage,
    enabled: true,
    action: 'block',
    logic: 'and',
    expression_mode: false,
    raw_expression: '',
    enrollment: { enabled: false, list_id: '', ttl_seconds: 3600, renew: true },
    response: {
      enabled: false,
      status: stage === 'rate' ? 429 : 403,
      content_type: 'application/json',
      body: '{"error":"Access denied"}',
    },
    conditions: [
      {
        id: crypto.randomUUID(),
        field: 'path',
        operator: 'starts_with',
        value: '',
      },
    ],
    skip: ['rate'],
    limit: 100,
    window: 60,
    duration: 600,
    hits: 0,
    signature: '',
  }
}
export function expression(rule: Rule): string {
  return rule.stage === 'managed'
    ? rule.signature
    : rule.expression_mode
      ? rule.raw_expression
      : builderExpression(rule.conditions, rule.logic)
}
