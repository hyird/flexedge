import { z } from 'zod'
import { createFileRoute } from '@tanstack/react-router'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'
import { ZoneDetailLayout } from '@/features/dns-zones/zone-detail-layout'

export const Route = createFileRoute('/_authenticated/dns-zones/$zoneId')({
  // `tab` omitted = 概览. q/type/source/edit belong to the records tab.
  validateSearch: listSearchSchema.pick({ q: true }).extend({
    tab: optionalSearchValue(z.enum(['records', 'lines'])),
    type: optionalSearchValue(z.enum(['A', 'AAAA', 'CNAME', 'TXT', 'MX'])),
    source: optionalSearchValue(z.enum(['system', 'custom'])),
    edit: optionalSearchValue(z.literal(1)),
  }),
  staticData: { breadcrumb: 'DNS 区域详情' },
  component: ZoneDetailLayout,
})
