import { createFileRoute } from '@tanstack/react-router'
import { SecurityRuleDetail } from '@/features/security/policy_pages'

export const Route = createFileRoute(
  '/_authenticated/security/policies/$policyId/rules/$ruleId'
)({ staticData: { breadcrumb: '规则配置' }, component: SecurityRuleDetail })
