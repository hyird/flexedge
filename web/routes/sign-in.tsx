import { createFileRoute, redirect } from '@tanstack/react-router'
import { sessionQueryOptions } from '@/lib/auth'
import { SignIn } from '@/features/auth/sign-in'

export const Route = createFileRoute('/sign-in')({
  validateSearch: (search: Record<string, unknown>) => {
    const candidate =
      typeof search.redirect === 'string' ? search.redirect : undefined
    return {
      redirect:
        candidate?.startsWith('/') && !candidate.startsWith('//')
          ? candidate
          : undefined,
    }
  },
  beforeLoad: async ({ context }) => {
    const authenticated = await context.queryClient
      .ensureQueryData(sessionQueryOptions)
      .then(() => true)
      .catch(() => false)

    if (authenticated) throw redirect({ to: '/' })
  },
  component: SignIn,
})
