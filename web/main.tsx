import { StrictMode } from 'react'
import ReactDOM from 'react-dom/client'
import { QueryClientProvider } from '@tanstack/react-query'
import { createRouter, RouterProvider } from '@tanstack/react-router'
import { toast } from 'sonner'
import { apiErrorMessage } from '@/lib/api'
import { createAppQueryClient } from '@/lib/query-client'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import { ThemeProvider } from '@/context/theme-provider'
import { createSessionExpiryHandler } from '@/features/auth/session-expiry'
import { routeTree } from './routeTree.gen'
import './styles/index.css'

const queryClient = createAppQueryClient(
  (message) => toast.error(message),
  (error) => handleSessionExpiry(error)
)
setLiveQueryQueryClient(queryClient, (error) => { handleSessionExpiry(error) })

const router = createRouter({
  routeTree,
  context: { queryClient },
  defaultPreload: 'intent',
  defaultPreloadStaleTime: 0,
})

const handleSessionExpiry = createSessionExpiryHandler(
  queryClient,
  () =>
    router.navigate({
      to: '/sign-in',
      search: { redirect: undefined },
      replace: true,
    }),
  (error) => toast.error(apiErrorMessage(error))
)

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
          <RouterProvider router={router} />
        </ThemeProvider>
      </QueryClientProvider>
    </StrictMode>
  )
}
