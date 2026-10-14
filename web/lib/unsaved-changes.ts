import {
  createContext,
  useContext,
  useEffect,
  useRef,
  useState,
  type RefObject,
} from 'react'
import { useBlocker } from '@tanstack/react-router'

export type UnsavedChangesGuard = {
  /** A navigation away is waiting for the user's decision. */
  isBlocked: boolean
  stay: () => void
  leave: () => void
}

/** Blocks every navigation (another page, an arrival on this page, reload,
 * browser tab close) while `isDirty`. In-page tab switches never touch the
 * URL; `PageTabs` guards them through `useTabLeaveGuard`. Render
 * `UnsavedChangesDialog` with the result, or use `SaveBar`, which does both. */
export function useUnsavedChangesGuard(isDirty: boolean): UnsavedChangesGuard {
  useTabLeaveGuard(isDirty)
  const blocker = useBlocker({
    shouldBlockFn: () => isDirty,
    enableBeforeUnload: () => isDirty,
    withResolver: true,
  })
  return {
    isBlocked: blocker.status === 'blocked',
    stay: () => blocker.reset?.(),
    leave: () => blocker.proceed?.(),
  }
}

/** Provided by `PageTabs`: whether the selected tab holds unsaved changes. */
export const TabLeaveGuardContext = createContext<RefObject<boolean> | null>(
  null
)

/** Marks the enclosing `PageTabs` tab dirty, so switching tabs asks first. */
function useTabLeaveGuard(isDirty: boolean) {
  const dirtyRef = useContext(TabLeaveGuardContext)
  useEffect(() => {
    if (!dirtyRef) return
    dirtyRef.current = isDirty
    return () => {
      dirtyRef.current = false
    }
  }, [dirtyRef, isDirty])
}

/** Guarded tab switching shared by `PageTabs` and same-page controls outside
 * it (header buttons such as `编辑配置`, `访问日志`, `编辑记录`). */
export type TabSwitchGuard = {
  /** Runs `switchTab` now, or after confirmation while the open tab's
   * `SaveBar` holds unsaved changes. */
  request: (switchTab: () => void) => void
  /** Set by the open tab's `SaveBar` (via `useUnsavedChangesGuard`). */
  dirtyRef: RefObject<boolean>
  /** A guarded switch waits for the user's decision. */
  isPending: boolean
  /** Discards the open tab's changes and runs the waiting switch. */
  confirm: () => void
  cancel: () => void
}

export function useTabSwitchGuard(): TabSwitchGuard {
  const dirtyRef = useRef(false)
  const [pending, setPending] = useState<{ run: () => void } | null>(null)
  return {
    dirtyRef,
    isPending: pending !== null,
    confirm: () => {
      dirtyRef.current = false
      setPending(null)
      pending?.run()
    },
    cancel: () => setPending(null),
    request: (switchTab) => {
      if (dirtyRef.current) setPending({ run: switchTab })
      else switchTab()
    },
  }
}
