import { useBlocker } from '@tanstack/react-router'

export type UnsavedChangesGuard = {
  /** A navigation away is waiting for the user's decision. */
  isBlocked: boolean
  stay: () => void
  leave: () => void
}

/** Blocks leaving the current page (other pathnames, reload, tab close) while
 * `isDirty`. Search-param changes on the same page, such as `?section=`, pass.
 * Render `UnsavedChangesDialog` with the result, or use `SaveBar`, which does both. */
export function useUnsavedChangesGuard(isDirty: boolean): UnsavedChangesGuard {
  const blocker = useBlocker({
    shouldBlockFn: ({ current, next }) =>
      isDirty && current.pathname !== next.pathname,
    enableBeforeUnload: () => isDirty,
    withResolver: true,
  })
  return {
    isBlocked: blocker.status === 'blocked',
    stay: () => blocker.reset?.(),
    leave: () => blocker.proceed?.(),
  }
}
