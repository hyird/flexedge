import { z } from 'zod'
import { createFileRoute } from '@tanstack/react-router'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'
import { ZoneRecords } from '@/features/dns-zones/zone-records'

export const Route = createFileRoute(
  '/_authenticated/dns-zones/$zoneId/records'
)({
  validateSearch: listSearchSchema.pick({ q: true }).extend({
    type: optionalSearchValue(z.enum(['A', 'AAAA', 'CNAME', 'TXT', 'MX'])),
    source: optionalSearchValue(z.enum(['system', 'custom'])),
    edit: optionalSearchValue(z.literal(1)),
  }),
  staticData: { breadcrumb: '记录' },
  component: ZoneRecords,
})
