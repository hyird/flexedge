import { createFileRoute } from '@tanstack/react-router'
import { ZoneDetailLayout } from '@/features/dns-zones/zone-detail-layout'

export const Route = createFileRoute('/_authenticated/dns-zones/$zoneId')({
  staticData: { breadcrumb: 'DNS 区域详情' },
  component: ZoneDetailLayout,
})
