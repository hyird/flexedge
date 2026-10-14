import { statusLabels } from '@/lib/status'
import type { Website, WebsiteOrigin } from './types'

/** `StatusChip` reserves: every label the website/deploy status can show. */
export const websiteStatusReserve = statusLabels(['enabled', 'disabled'])
export const deployStatusReserve = statusLabels([
  'applied',
  'pending',
  'partial',
  'no_nodes',
])

/** Display name used in titles, menus and confirmations. */
export function websiteName(
  website: Pick<Website, 'config' | 'access_domain'>
) {
  return (
    website.config.name ||
    website.config.domains[0]?.hostname ||
    website.access_domain ||
    '未命名网站'
  )
}

/** Primary origin of the default origin group, falling back to its first member. */
export function defaultOrigin(website: Pick<Website, 'config'>) {
  const group = website.config.origins.filter(
    (origin) => origin.group === website.config.default_origin_group
  )
  return group.find((origin) => origin.role === 'primary') ?? group[0]
}

export function originAddress(
  origin: Pick<WebsiteOrigin, 'protocol' | 'host' | 'port'>
) {
  return `${origin.protocol}://${origin.host}:${origin.port}`
}

/** Origin groups in configuration order. */
export function originGroups(website: Pick<Website, 'config'>) {
  return [...new Set(website.config.origins.map((origin) => origin.group))]
}

/** Health reports by status: `healthy` counts as up, anything else as down. */
export function originHealth(website: Pick<Website, 'runtime'>) {
  const states = website.runtime.origin_states
  return {
    total: states.length,
    healthy: states.filter((state) => state.status === 'healthy').length,
  }
}
