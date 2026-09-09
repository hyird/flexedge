import { useQueries } from '@tanstack/react-query'
import { dnsZoneLinesQuery } from '@/features/dns-zones/data'
import type { DnsLine } from '@/features/dns-zones/types'

export function useClusterDnsLines(
  clusters: ReadonlyArray<{ id: string; dns_zone_id: string }>
) {
  const zoneIds = [...new Set(clusters.map((cluster) => cluster.dns_zone_id))]
  return useQueries({
    queries: zoneIds.map(dnsZoneLinesQuery),
    combine: (results) =>
      Object.fromEntries(
        clusters.map((cluster) => [
          cluster.id,
          results[zoneIds.indexOf(cluster.dns_zone_id)]?.data ?? [],
        ])
      ) as Record<string, DnsLine[]>,
  })
}
