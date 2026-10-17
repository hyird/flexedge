import { useLocation, useRouter, useSearch } from '@tanstack/react-router'
import type { SecuritySearch } from './security_search'

export function useSecuritySearch() {
  const search = useSearch({ strict: false }) as SecuritySearch
  const router = useRouter()
  const pathname = useLocation({ select: (location) => location.pathname })
  const patch = (
    changes: Partial<Record<keyof SecuritySearch, string | number>>,
    push = false
  ) => {
    const next = { ...search, ...changes },
      params = new URLSearchParams()
    for (const [key, value] of Object.entries(next))
      if (
        value !== undefined &&
        value !== '' &&
        !(key === 'page' && value === 1)
      )
        params.set(key, String(value))
    void router.navigate({
      href: `${pathname}${params.size ? `?${params}` : ''}`,
      replace: !push,
      resetScroll: false,
    })
  }
  return { search, patch, router }
}
