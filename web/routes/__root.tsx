import type { QueryClient } from '@tanstack/react-query'
import { createRootRouteWithContext, Outlet } from '@tanstack/react-router'
import { Toast } from '@heroui/react'
import { GeneralError } from '@/features/errors/general-error'
import { NotFoundError } from '@/features/errors/not-found-error'

export const Route = createRootRouteWithContext<{ queryClient: QueryClient }>()(
  {
    component: () => (
      <>
        <Outlet />
        <Toast.Provider placement='top end' width={340} gap={8} />
      </>
    ),
    errorComponent: GeneralError,
    notFoundComponent: NotFoundError,
  }
)
