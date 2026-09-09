import { isAxiosError } from 'axios'
import type { QueryClient } from '@tanstack/react-query'
import { queryKeys } from '@/lib/query-keys'
import { replaceSession, sessionVersion } from './session-lifecycle'
import { sessionQueryOptions } from './data'

/** The API client has already attempted refresh before reporting a final 401. */
export function createSessionExpiryHandler(
  client: QueryClient,
  onExpired: () => void | Promise<void>,
  onError: (error: unknown) => void
) {
  let expiring = false
  return (error: unknown) => {
    if (!isAxiosError(error) || error.response?.status !== 401) return false
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

/** A stream has no Axios response to trigger the normal 401 interceptor. Probe
 * once through the same session query so refresh is shared by the API client;
 * only a failed probe ends the session. */
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
    if (!isAxiosError(error) || error.response?.status !== 401) return true
    await replaceSession(client)
    return false
  }
}
