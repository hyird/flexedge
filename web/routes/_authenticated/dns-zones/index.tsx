import { z } from 'zod'
import { createFileRoute } from '@tanstack/react-router'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'
import { zoneSyncFilterIds } from '@/features/dns-zones/dns-zone-display'
import { DnsZonesPage } from '@/features/dns-zones/zones-page'

export const Route = createFileRoute('/_authenticated/dns-zones/')({
  validateSearch: listSearchSchema.extend({
    dns_provider_id: optionalSearchValue(z.string().uuid()),
    sync_status: optionalSearchValue(z.enum(zoneSyncFilterIds)),
    create: optionalSearchValue(z.literal(1)),
  }),
  staticData: { breadcrumb: '域名管理' },
  component: DnsZonesPage,
})
