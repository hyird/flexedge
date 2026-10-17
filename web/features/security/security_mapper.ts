import type { z } from 'zod'
import type { managedRuleSchema, securitySharedListSchema } from './data'
import {
  expressionToBuilder,
  parseExpression,
  referencedLists,
} from './expression_model'
import type { SecurityList } from './lists_model'
import { expression, newRule, type Rule } from './model'
import {
  defaultSecurityPolicy,
  type SecurityPolicy as NativePolicy,
  type SecurityPolicyInput,
  type CustomRule,
  type CcRule,
} from './transport'

export type SecurityPolicy = {
  id: string
  name: string
  description: string
  revision: number
  managed: { paranoiaLevel: number; inboundThreshold: number }
  rules: Rule[]
}
export type ManagedRule = z.infer<typeof managedRuleSchema>
export type SharedList = SecurityList & { revision: number }
export function listFromApi(
  list: z.infer<typeof securitySharedListSchema>
): SharedList {
  return {
    ...list,
    members: list.members.map((member) => ({
      id: member.id,
      value: member.value,
      note: member.note,
      source: member.source,
      ruleId: member.rule_id || undefined,
      addedAt: member.added_unix_ms,
      expiresAt: member.expires_unix_ms || null,
    })),
  }
}
function ruleFromApi(
  rule: CustomRule | CcRule,
  stage: 'custom' | 'rate'
): Rule {
  const value = {
    ...newRule(stage),
    id: rule.id,
    name: rule.name || rule.id,
    enabled: rule.enabled,
    action: rule.action,
    raw_expression: rule.expression,
  }
  try {
    Object.assign(value, expressionToBuilder(parseExpression(rule.expression)))
    value.expression_mode = rule.expression_mode ?? false
  } catch {
    value.expression_mode = true
  }
  value.response = {
    enabled: !!rule.response_body,
    status: rule.response_status ?? (stage === 'rate' ? 429 : 403),
    content_type: rule.response_content_type || 'application/json',
    body: rule.response_body ?? '',
  }
  if (stage === 'rate') {
    const cc = rule as CcRule
    value.limit = cc.requests
    value.window = cc.window_seconds
    value.duration = cc.mitigation_seconds
  }
  if ('skip_stages' in rule)
    value.skip = rule.skip_stages ?? ['custom', 'rate', 'managed']
  return value
}
export function policyFromApi(
  policy: NativePolicy,
  catalog: ManagedRule[]
): SecurityPolicy {
  const managed = policy.config.managed
  return {
    id: policy.id,
    name: policy.name,
    description: policy.description,
    revision: policy.revision,
    managed: {
      paranoiaLevel: managed.paranoia_level,
      inboundThreshold: managed.inbound_threshold,
    },
    rules: [
      ...policy.config.custom_rules.map((rule) => ruleFromApi(rule, 'custom')),
      ...policy.config.cc_rules.map((rule) => ruleFromApi(rule, 'rate')),
      ...catalog.map((rule) => ({
        ...newRule('managed'),
        id: String(rule.id),
        name: rule.name,
        signature: `OWASP CRS 4.25.0 · ${rule.id} · ${rule.category} · PL${rule.paranoia_level}`,
        enabled: managed.enabled && !managed.excluded_rules.includes(rule.id),
        action: managed.detection_only ? ('log' as const) : ('block' as const),
      })),
    ].map((rule) => ({
      ...rule,
      conditions: rule.conditions.map((condition, index) => ({
        ...condition,
        id: `${rule.id}:${index}`,
      })),
    })),
  }
}
export function policyToApi(
  policy: SecurityPolicy,
  lists: SecurityList[],
  catalog: ManagedRule[],
  previous?: NativePolicy
): SecurityPolicyInput {
  const config = structuredClone(
    previous?.config ?? defaultSecurityPolicy().config
  )
  const references = new Set<string>()
  const base = (rule: Rule) => {
    if (rule.enrollment.enabled)
      throw new Error('当前节点尚未支持自动入列，不能部署此配置')
    if (rule.action === 'challenge')
      throw new Error('当前节点尚未接入挑战验证，不能部署此动作')
    if (rule.stage !== 'managed')
      for (const name of referencedLists(parseExpression(expression(rule))))
        references.add(name)
    return {
      id: rule.id,
      name: rule.name,
      enabled: rule.enabled,
      expression: expression(rule),
      expression_mode: rule.expression_mode,
      action: rule.action,
      ...(rule.action === 'block' && rule.response.enabled
        ? {
            response_status: rule.response.status,
            response_content_type: rule.response.content_type,
            response_body: rule.response.body,
          }
        : {}),
    }
  }
  config.custom_rules = policy.rules
    .filter((rule) => rule.stage === 'custom')
    .map((rule) => ({
      ...base(rule),
      action: rule.action as CustomRule['action'],
      skip_stages: rule.skip,
    }))
  config.cc_rules = policy.rules
    .filter((rule) => rule.stage === 'rate')
    .map((rule) => ({
      ...base(rule),
      action: rule.action as CcRule['action'],
      requests: rule.limit,
      window_seconds: rule.window,
      mitigation_seconds: rule.duration,
    }))
  const managed = policy.rules.filter((rule) => rule.stage === 'managed')
  for (const rule of managed) {
    base(rule)
    if (rule.response.enabled) throw new Error('CRS 单条规则暂不支持自定义响应')
    if (
      catalog.find((entry) => String(entry.id) === rule.id)?.name !== rule.name
    )
      throw new Error('CRS 规则名称由规则集提供，不能修改')
  }
  const active = managed.filter((rule) => rule.enabled)
  if (
    active.some((rule) => rule.action === 'log') &&
    active.some((rule) => rule.action === 'block')
  )
    throw new Error('CRS 仅记录模式当前按整套规则集配置，不能混用逐条动作')
  const identities = new Set(catalog.map((rule) => rule.id))
  config.managed = {
    ...config.managed,
    paranoia_level: policy.managed.paranoiaLevel,
    inbound_threshold: policy.managed.inboundThreshold,
    enabled: active.length > 0,
    detection_only:
      active.length > 0 && active.every((rule) => rule.action === 'log'),
    excluded_rules: [
      ...config.managed.excluded_rules.filter((id) => !identities.has(id)),
      ...managed.filter((rule) => !rule.enabled).map((rule) => Number(rule.id)),
    ],
  }
  config.lists = [...references].map((name) => {
    const list = lists.find((item) => item.name === name)
    if (!list) throw new Error(`清单 $${name} 不存在`)
    return {
      name,
      type: list.kind === 'ua' ? 'user_agent' : list.kind,
      entries: [],
    }
  })
  return { name: policy.name, description: policy.description, config }
}
