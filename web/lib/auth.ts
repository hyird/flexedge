import { queryOptions } from '@tanstack/react-query'
import { getData, sendData } from '@/lib/api'
import type { AuthUser } from '@/lib/types'

export const sessionQueryOptions = queryOptions({
  queryKey: ['session'],
  queryFn: () => getData<AuthUser>('/auth/me'),
  retry: false,
  staleTime: 30_000,
})

export async function login(username: string, password: string) {
  const response = await sendData<{ user: AuthUser }>('post', '/auth/login', {
    username,
    password,
  })
  return response.data.user
}

export async function logout() {
  await sendData('post', '/auth/logout')
}
