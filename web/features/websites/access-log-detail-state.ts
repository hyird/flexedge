import type { AccessLog } from './access-log-schema'

export function updateAccessLogDetailState(
  current: ReadonlySet<string>,
  id: string,
  open: boolean,
  currentLiveLogs: readonly AccessLog[],
  frozenLiveLogs: readonly AccessLog[] | null
) {
  const next = new Set(current)
  if (open) next.add(id)
  else next.delete(id)

  return {
    openDetailIds: next,
    frozenLiveLogs:
      next.size === 0
        ? null
        : current.size === 0
          ? [...currentLiveLogs]
          : frozenLiveLogs
            ? [...frozenLiveLogs]
            : null,
  }
}
