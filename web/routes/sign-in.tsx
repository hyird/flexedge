import { createFileRoute, redirect } from '@tanstack/react-router'
import { SignInPage } from '@/features/auth/sign-in'

export const Route = createFileRoute('/sign-in')({
  validateSearch: (search: Record<string, unknown>) => ({
    redirect: typeof search.redirect === 'string' ? search.redirect : undefined,
  }),
  beforeLoad: ({ search }) => {
    if (search.redirect === '/')
      throw redirect({
        to: '/sign-in',
        search: { redirect: undefined },
        replace: true,
      })
  },
  component: SignInPage,
})
