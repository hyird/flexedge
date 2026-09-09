import { AxiosError } from 'axios'
import { QueryCache, QueryClient } from '@tanstack/react-query'
import { apiErrorMessage } from './api'
import { ApiBusinessError, ApiProtocolError } from './api-response'

export function createAppQueryClient(
  notifyError: (message: string) => void,
  handleError?: (error: unknown) => boolean
) {
  return new QueryClient({
    defaultOptions: {
      queries: {
        retry: (failureCount, error) =>
          !(
            error instanceof ApiBusinessError ||
            error instanceof ApiProtocolError
          ) &&
          !(
            error instanceof AxiosError &&
            [401, 403].includes(error.status ?? 0)
          ) &&
          failureCount < 2,
        refetchOnWindowFocus: false,
        // Mutations and server events own resource freshness. Remounting or
        // restoring connectivity must not introduce a background refresh loop.
        staleTime: Infinity,
        refetchOnReconnect: false,
      },
      mutations: {
        onError: (error) => {
          if (!handleError?.(error)) notifyError(apiErrorMessage(error))
        },
      },
    },
    queryCache: new QueryCache({
      onError: (error, query) => {
        if (handleError?.(error)) return
        if (
          query.meta?.unauthorizedIsExpected === true &&
          error instanceof AxiosError &&
          error.response?.status === 401
        )
          return
        if (query.state.data === undefined) notifyError(apiErrorMessage(error))
      },
    }),
  })
}
