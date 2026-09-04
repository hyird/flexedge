import axios, {
  type AxiosError,
  type InternalAxiosRequestConfig,
  type RawAxiosRequestHeaders,
} from 'axios'

export type ApiEnvelope<T> = {
  code: number
  message: string
  data: T
}

export type PageData<T> = {
  list: T[]
  total: number
  page: number
  page_size: number
  total_pages: number
}

type RetryableRequest = InternalAxiosRequestConfig & { _retried?: boolean }

export const api = axios.create({
  baseURL: '/api',
  timeout: 20_000,
  withCredentials: true,
})

let refreshRequest: Promise<void> | null = null

export function normalizeApiPath(url: string) {
  const normalized = url.replace(/\/+((?:[?#].*)?)$/, '$1')
  return normalized || '/'
}

api.interceptors.response.use(undefined, async (error: AxiosError) => {
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
})

export async function getData<T>(
  url: string,
  params?: Record<string, unknown>
) {
  const response = await api.get<ApiEnvelope<T>>(normalizeApiPath(url), {
    params,
  })
  return response.data.data
}

export async function sendData<T = unknown>(
  method: 'post' | 'put' | 'delete',
  url: string,
  data?: unknown,
  revision?: number
) {
  const headers: RawAxiosRequestHeaders | undefined =
    revision === undefined ? undefined : { 'If-Match': `"${revision}"` }
  const response = await api.request<ApiEnvelope<T>>({
    method,
    url: normalizeApiPath(url),
    data,
    headers,
  })
  return response.data
}

export function apiErrorMessage(error: unknown) {
  if (axios.isAxiosError<ApiEnvelope<unknown>>(error)) {
    const message = error.response?.data?.message
    if (message) return message
    if (error.code === 'ECONNABORTED') return '请求超时，请稍后重试'
    if (!error.response) return '无法连接 FlexEdge 服务'
    if (error.response.status === 401) return '登录状态已失效，请重新登录'
    if (error.response.status === 403) return '当前账户没有操作权限'
    if (error.response.status === 409) return '数据已更新，请刷新后重试'
    if (error.response.status >= 500) return 'FlexEdge 服务暂时不可用'
    if (error.response.status >= 400) return '请求未能完成，请检查输入后重试'
  }
  if (error instanceof Error && error.message) return error.message
  return '操作失败，请稍后重试'
}
