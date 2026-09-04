import { StrictMode } from 'react'
import ReactDOM from 'react-dom/client'
import { AxiosError } from 'axios'
import {
  QueryCache,
  QueryClient,
  QueryClientProvider,
} from '@tanstack/react-query'
import { createRouter, RouterProvider } from '@tanstack/react-router'
import { toast } from 'sonner'
import { apiErrorMessage } from '@/lib/api'
import { DirectionProvider } from '@/context/direction-provider'
import { FontProvider } from '@/context/font-provider'
import { ThemeProvider } from '@/context/theme-provider'
import { routeTree } from './routeTree.gen'
import './styles/index.css'

const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      retry: (failureCount, error) =>
        !(
          error instanceof AxiosError && [401, 403].includes(error.status ?? 0)
        ) && failureCount < 2,
      refetchOnWindowFocus: false,
      staleTime: 10_000,
    },
    mutations: {
      onError: (error) => toast.error(apiErrorMessage(error)),
    },
  },
  queryCache: new QueryCache({
    onError: (error, query) => {
      if (query.state.data === undefined) toast.error(apiErrorMessage(error))
    },
  }),
})

const router = createRouter({
  routeTree,
  context: { queryClient },
  defaultPreload: 'intent',
  defaultPreloadStaleTime: 0,
})

declare module '@tanstack/react-router' {
  interface Register {
    router: typeof router
  }
}

const rootElement = document.getElementById('root')

if (!rootElement) throw new Error('Missing root element')

if (!rootElement.innerHTML) {
  ReactDOM.createRoot(rootElement).render(
    <StrictMode>
      <QueryClientProvider client={queryClient}>
        <ThemeProvider>
          <FontProvider>
            <DirectionProvider>
              <RouterProvider router={router} />
            </DirectionProvider>
          </FontProvider>
        </ThemeProvider>
      </QueryClientProvider>
    </StrictMode>
  )
}
