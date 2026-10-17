import { queryOptions, type QueryClient } from '@tanstack/react-query'
import { getData, sendData } from '@/lib/api'
import { parseResponseData } from '@/lib/api-response'
import { queryKeys } from '@/lib/query-keys'
import { authUserSchema } from '@/features/auth/types'
import { replaceSession, sessionVersion } from './session-lifecycle'

export const sessionQueryOptions = queryOptions({
  queryKey: queryKeys.session,
  // Signed-out routes probe the session too; their 401 is expected.
  meta: { unauthorizedIsExpected: true },
  queryFn: async ({ signal }) =>
    parseResponseData(
      authUserSchema,
      await getData<unknown>('/auth/me', undefined, signal)
    ),
  retry: false,
  staleTime: 30_000,
})

export async function ensureAuthenticatedSession(client: QueryClient) {
  const version = sessionVersion(client)
  const user = await client.ensureQueryData(sessionQueryOptions)
  if (sessionVersion(client) !== version)
    throw new Error('登录会话已变更，请重试')
  return user
}

export async function login(
  client: QueryClient,
  username: string,
  password: string
) {
  const response = await sendData<{ user?: unknown }>('post', '/auth/login', {
    username,
    password,
  })
  const user = parseResponseData(authUserSchema, response.data?.user)
  await replaceSession(client, user)
  return user
}

export async function logout(client: QueryClient) {
  await sendData('post', '/auth/logout')
  await replaceSession(client)
}
