import { z } from 'zod'
import { keepPreviousData, queryOptions } from '@tanstack/react-query'
import { Globe, Layers, Server, ShieldCheck, Signal } from '@gravity-ui/icons'
import { streamPath } from '@/lib/api'
import { ApiProtocolError } from '@/lib/api-response'
import { readLiveSnapshot } from '@/lib/live-query'

type ResourceSearchHit = {
  key: string
  label: string
  href: string
}
type HitFields = { id: string; label: string }

type ResourceSearchSource = {
  label: string
  icon: typeof Globe
  /** List endpoint and detail route prefix. */
  path: string
  parse: (value: unknown) => HitFields[]
}

// Only the fields shown in the palette are validated; list DTOs stay owned by
// their domains. Binding schema and mapping here keeps each pair type-checked.
function defineSource<T>(
  source: Omit<ResourceSearchSource, 'parse'> & {
    item: z.ZodType<T>
    toHit: (item: T) => HitFields
  }
): ResourceSearchSource {
  const { item, toHit, ...rest } = source
  const schema = z.object({ list: z.array(item) })
  return {
    ...rest,
    parse: (value) => {
      const result = schema.safeParse(value)
      if (!result.success) throw new ApiProtocolError()
      return result.data.list.map(toHit)
    },
  }
}

export const resourceSearchSources: ResourceSearchSource[] = [
  defineSource({
    label: '网站',
    icon: Globe,
    path: '/websites',
    item: z.object({
      id: z.string(),
      config: z.object({
        name: z.string().optional(),
        domains: z.array(z.object({ hostname: z.string() })),
      }),
    }),
    toHit: (website) => ({
      id: website.id,
      label:
        website.config.name ||
        website.config.domains[0]?.hostname ||
        website.id,
    }),
  }),
  defineSource({
    label: '节点',
    icon: Server,
    path: '/nodes',
    item: z.object({ id: z.string(), name: z.string() }),
    toHit: (node) => ({ id: node.id, label: node.name }),
  }),
  defineSource({
    label: '集群',
    icon: Layers,
    path: '/clusters',
    item: z.object({ id: z.string(), name: z.string() }),
    toHit: (cluster) => ({ id: cluster.id, label: cluster.name }),
  }),
  defineSource({
    label: '证书',
    icon: ShieldCheck,
    path: '/certificates',
    item: z.object({ id: z.string(), domains: z.array(z.string()) }),
    toHit: (certificate) => ({
      id: certificate.id,
      label: certificate.domains[0] ?? certificate.id,
    }),
  }),
  defineSource({
    label: '域名',
    icon: Signal,
    path: '/dns-zones',
    item: z.object({ id: z.string(), domain: z.string() }),
    toHit: (zone) => ({ id: zone.id, label: zone.domain }),
  }),
]

/** One-shot keyword search over a list stream (first 5 matches). The stream
 * closes after the first snapshot; `gcTime: 0` drops the result once the
 * palette closes, so the next opening reads a fresh snapshot. */
export function resourceSearchQuery(
  source: ResourceSearchSource,
  keyword: string
) {
  return queryOptions({
    queryKey: ['command-search', source.path, keyword],
    enabled: keyword.length > 0,
    retry: false,
    gcTime: 0,
    placeholderData: keepPreviousData,
    queryFn: ({ queryKey, signal }): Promise<ResourceSearchHit[]> =>
      readLiveSnapshot(
        streamPath(source.path, { keyword, page: 1, page_size: 5 }),
        queryKey,
        signal,
        (value) =>
          source.parse(value).map((hit) => ({
            key: `${source.path}/${hit.id}`,
            label: hit.label,
            href: `${source.path}/${hit.id}`,
          }))
      ),
  })
}
