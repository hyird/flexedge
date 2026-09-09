import axios, { type AxiosError, type InternalAxiosRequestConfig } from 'axios'
import { parseApiResponse } from './api-response'

type RetryableRequest = InternalAxiosRequestConfig & { _retried?: boolean }

export function createApiClient() {
  const api = axios.create({
    baseURL: '/api',
    timeout: 20_000,
    withCredentials: true,
  })
  let refreshRequest: Promise<void> | null = null

  api.interceptors.response.use(
    (response) => {
      if (
        response.config.responseType !== 'blob' &&
        response.config.responseType !== 'arraybuffer'
      ) {
        response.data = parseApiResponse(response.data)
      }
      return response
    },
    async (error: AxiosError) => {
      const request = error.config as RetryableRequest | undefined
      const path = request?.url ?? ''
      const canRefresh =
        error.response?.status === 401 &&
        request &&
        !request._retried &&
        !path.includes('/auth/login') &&
        !path.includes('/auth/refresh')

      if (!canRefresh) throw error

      request._retried = true
      refreshRequest ??= api
        .post('/auth/refresh')
        .then(() => undefined)
        .finally(() => {
          refreshRequest = null
        })

      await refreshRequest
      return api(request)
    }
  )

  return api
}
