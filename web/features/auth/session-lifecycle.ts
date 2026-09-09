import type { QueryClient } from '@tanstack/react-query'
import { queryKeys } from '@/lib/query-keys'
import { closeLiveQueries } from '@/lib/live-query'
import type { AuthUser } from './types'

type Lifetime = { active: boolean; version: number; cleanups: Set<() => void> }
const lifetimes = new WeakMap<QueryClient, Lifetime>()

function lifetimeFor(client: QueryClient) {
  let lifetime = lifetimes.get(client)
  if (!lifetime) {
    lifetime = { active: true, version: 0, cleanups: new Set() }
    lifetimes.set(client, lifetime)
  }
  return lifetime
}

export function sessionVersion(client: QueryClient) {
  return lifetimeFor(client).version
}

/** A route may re-enter after /auth/me confirms an externally established session. */
export function activateSession(client: QueryClient, version: number) {
  const lifetime = lifetimeFor(client)
  if (lifetime.version !== version) return false
  lifetime.active = true
  return true
}

/** Resources stop synchronously at the session boundary, before router unmount. */
export function registerSessionCleanup(
  client: QueryClient,
  cleanup: () => void
) {
  const lifetime = lifetimeFor(client)
  let disposed = false
  const dispose = () => {
    if (disposed) return
    disposed = true
    lifetime.cleanups.delete(dispose)
    cleanup()
  }
  if (lifetime.active) lifetime.cleanups.add(dispose)
  else dispose()
  return dispose
}

/** Called only after the server accepts an explicit login or logout. */
export async function replaceSession(client: QueryClient, user?: AuthUser) {
  const lifetime = lifetimeFor(client)
  lifetime.version++
  lifetime.active = false
  for (const dispose of lifetime.cleanups) dispose()
  closeLiveQueries()
  // Cancellation prevents late query completions from publishing even when a
  // transport ignores AbortSignal. Remove all resources, not just /auth/me.
  await client.cancelQueries()
  client.removeQueries()
  if (user) {
    client.setQueryData(queryKeys.session, user)
    lifetime.active = true
  }
}
