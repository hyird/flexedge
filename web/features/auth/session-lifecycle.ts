import type { QueryClient } from '@tanstack/react-query'
import { closeLiveQueries } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import type { AuthUser } from './types'

const versions = new WeakMap<QueryClient, number>()

/** Changes whenever the session is replaced (login, logout, expiry). */
export function sessionVersion(client: QueryClient) {
  return versions.get(client) ?? 0
}

/** Called only after the server accepts an explicit login or logout. Live
 * views close synchronously at the session boundary, before router unmount. */
export async function replaceSession(client: QueryClient, user?: AuthUser) {
  versions.set(client, sessionVersion(client) + 1)
  closeLiveQueries()
  // Cancellation prevents late query completions from publishing even when a
  // transport ignores AbortSignal. Remove all resources, not just /auth/me.
  await client.cancelQueries()
  client.removeQueries()
  if (user) client.setQueryData(queryKeys.session, user)
}
