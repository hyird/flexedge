import { useState } from 'react'

// Editing controls must not change the active server query until applied.
export function useResourceFilters<T extends Record<string, string>>(
  initial: T
) {
  const [draft, setDraft] = useState(initial)
  const [filters, setFilters] = useState(initial)

  function apply() {
    const next = Object.fromEntries(
      Object.entries(draft).map(([key, value]) => [key, value.trim()])
    ) as T
    const changed = Object.keys(next).some((key) => next[key] !== filters[key])
    setFilters(next)
    return changed
  }

  function reset() {
    const changed = Object.keys(initial).some(
      (key) => filters[key] !== initial[key]
    )
    setDraft(initial)
    setFilters(initial)
    return changed
  }

  return {
    draft,
    filters,
    setField: (key: keyof T, value: string) =>
      setDraft((current) => ({ ...current, [key]: value })),
    apply,
    reset,
  }
}
