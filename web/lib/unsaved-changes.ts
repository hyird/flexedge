import { useBlocker } from '@tanstack/react-router'

export type UnsavedChangesGuard = {
  /** A navigation away is waiting for the user's decision. */
  isBlocked: boolean
  stay: () => void
  leave: () => void
}

/** The in-page tab a location shows (`?tab=`); undefined = the default tab. */
function searchTab(search: object) {
  return 'tab' in search ? search.tab : undefined
}

/** Blocks leaving the current page or in-page tab (other pathnames, another
 * `?tab=`, reload, tab close) while `isDirty`. Other search-param changes on
 * the same tab, such as `?section=`, pass. Render `UnsavedChangesDialog` with
 * the result, or use `SaveBar`, which does both. */
export function useUnsavedChangesGuard(isDirty: boolean): UnsavedChangesGuard {
  const blocker = useBlocker({
    shouldBlockFn: ({ current, next }) =>
      isDirty &&
      (current.pathname !== next.pathname ||
        searchTab(current.search) !== searchTab(next.search)),
    enableBeforeUnload: () => isDirty,
    withResolver: true,
  })
  return {
    isBlocked: blocker.status === 'blocked',
    stay: () => blocker.reset?.(),
    leave: () => blocker.proceed?.(),
  }
}
