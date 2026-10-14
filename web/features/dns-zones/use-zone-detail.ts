import { useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { dnsZoneDetailQuery } from './data'

const route = getRouteApi('/_authenticated/dns-zones/$zoneId')

/** The zone of the current detail route. Layout and tabs share one query and
 * therefore one `/dns-zones/:id/stream` connection. */
export function useZoneDetail() {
  const { zoneId } = route.useParams()
  return useQuery(dnsZoneDetailQuery(zoneId))
}
