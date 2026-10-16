import { createFileRoute } from '@tanstack/react-router'
import { zoneSearchSchema } from '@/features/dns-zones/zone-search'
import { DnsZonesPage } from '@/features/dns-zones/zones-page'

export const Route = createFileRoute('/_authenticated/dns-zones/')({
  validateSearch: zoneSearchSchema,
  staticData: { breadcrumb: '域名管理' },
  component: DnsZonesPage,
})
