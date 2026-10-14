import { AxiosError } from 'axios'
import { QueryCache, QueryClient } from '@tanstack/react-query'
import { apiErrorMessage } from './api'
import { ApiBusinessError, ApiProtocolError } from './api-response'

/** Query/mutation `meta` understood by the shared error handling. */
declare module '@tanstack/react-query' {
  interface Register {
    queryMeta: {
      /** A 401 is a normal answer (session probe on signed-out routes). */
      unauthorizedIsExpected?: boolean
      /** The view renders this error itself (empty/not-found state, Alert). */
      inlineError?: boolean
    }
    mutationMeta: {
      /** The form, SaveBar or ConfirmDialog shows this error inline. */
      inlineError?: boolean
    }
  }
}

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
        onError: (error, _variables, _result, context) => {
          if (handleError?.(error) || context.meta?.inlineError) return
          notifyError(apiErrorMessage(error))
        },
      },
    },
    queryCache: new QueryCache({
      onError: (error, query) => {
        if (handleError?.(error) || query.meta?.inlineError) return
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
