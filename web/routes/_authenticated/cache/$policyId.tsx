import { createFileRoute } from '@tanstack/react-router'
import { CachePolicyDetail } from '@/features/cache/cache_policy_detail'

export const Route = createFileRoute('/_authenticated/cache/$policyId')({
  staticData: { breadcrumb: '缓存策略' },
  component: CachePolicyDetail,
})
