import { createFileRoute } from '@tanstack/react-router'
import { SecurityPolicyDetail } from '@/features/security/policy_pages'

export const Route = createFileRoute(
  '/_authenticated/security/policies/$policyId/'
)({ component: SecurityPolicyDetail })
