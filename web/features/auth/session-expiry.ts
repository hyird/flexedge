import type { QueryClient } from '@tanstack/react-query'
import { isUnauthorized } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import { sessionQueryOptions } from './data'
import { replaceSession, sessionVersion } from './session-lifecycle'

/** Business requests try refresh; a session probe's 401 is already final. */
export function createSessionExpiryHandler(
  client: QueryClient,
  onExpired: () => void | Promise<void>,
  onError: (error: unknown) => void
) {
  let expiring = false
  return (error: unknown) => {
    if (!isUnauthorized(error)) return false
    if (expiring) return true
    // Initial login failures belong to their form/route, not session teardown.
    if (client.getQueryData(queryKeys.session) === undefined) return false
    expiring = true
    void replaceSession(client)
      .then(onExpired)
      .catch(onError)
      .finally(() => {
        expiring = false
      })
    return true
  }
}

/** A stream probes the same session once; an unauthorized probe ends it. */
export async function recoverStreamSession(client: QueryClient) {
  const version = sessionVersion(client)
  try {
    await client.fetchQuery({ ...sessionQueryOptions, staleTime: 0 })
    if (sessionVersion(client) !== version) return false
    return true
  } catch (error) {
    if (sessionVersion(client) !== version) return false
    // A dropped network connection is not proof of an expired session. Let
    // the stream's reconnect path try again without clearing a valid cache.
    if (!isUnauthorized(error)) return true
    await replaceSession(client)
    return false
  }
}
