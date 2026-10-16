import { createFileRoute, redirect } from '@tanstack/react-router'

/** `/providers` opens its DNS account list. */
export const Route = createFileRoute('/_authenticated/providers/')({
  beforeLoad: () => {
    throw redirect({ to: '/providers/dns', search: {}, replace: true })
  },
})
