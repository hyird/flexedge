import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { QueryClientProvider } from '@tanstack/react-query'
import { createRouter, RouterProvider } from '@tanstack/react-router'
import { toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import { createAppQueryClient } from '@/lib/query-client'
import { ThemeProvider } from '@/context/theme-provider'
import { createSessionExpiryHandler } from '@/features/auth/session-expiry'
import { routeTree } from '../build/generated/web/routeTree.gen'
import './styles/index.css'

const queryClient = createAppQueryClient(
  (message) => toast.danger(message),
  (error) => handleSessionExpiry(error)
)
setLiveQueryQueryClient(queryClient, (error) => {
  handleSessionExpiry(error)
})
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
  (error) => toast.danger(apiErrorMessage(error))
)
declare module '@tanstack/react-router' {
  interface Register {
    router: typeof router
  }
}
const element = document.getElementById('root')
if (!element) throw new Error('缺少应用容器')
createRoot(element).render(
  <StrictMode>
    <QueryClientProvider client={queryClient}>
      <ThemeProvider>
        <RouterProvider router={router} />
      </ThemeProvider>
    </QueryClientProvider>
  </StrictMode>
)
