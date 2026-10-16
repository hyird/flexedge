import { useCallback, useEffect, useState } from 'react'
import { useMutation } from '@tanstack/react-query'
import { isRevisionConflict } from './api'

type Revisioned = { revision: number }

export type RevisionDraft<T extends Revisioned, V> = {
  /** Snapshot the draft is based on: the latest one while the draft is clean,
   * the one it started from while dirty. Editors derive their clean values
   * from it (RHF: the `values` option). */
  base: T | undefined
  /** Values of the last save until `base` reaches the revision it produced;
   * editors that keep their own values use them instead of `base`'s. */
  savedValues: V | undefined
  /** `If-Match` revision of the next save. */
  revision: number | undefined
  /** Someone else changed the resource under the dirty draft. */
  stale: boolean
  /** The last save lost a revision race. */
  conflict: boolean
  /** Error of the last save. */
  error: Error | null
  isSaving: boolean
  /** Reports whether the draft differs from its clean values: RHF forms from
   * `subscribe` (`useDraftForm`), other editors on every change. */
  track: (dirty: boolean) => void
  /** Saves the draft against `revision`. */
  save: (values: V) => void
  /** `save` for confirmations that wait for the write (rejects on failure). */
  saveAsync: (values: V) => Promise<unknown>
  /** Drops the draft's changes for the latest snapshot. */
  discard: () => void
  /** Re-reads the resource and drops the draft's changes. */
  reload: () => Promise<void>
  isReloading: boolean
}

/** The console's one draft editor of a server resource saved with `If-Match`:
 * a clean draft follows the server; a dirty draft keeps the snapshot it
 * started from (`base`), so a concurrent change shows as `stale` and saving
 * it fails with a revision conflict instead of overwriting it. After a save
 * the draft is based on the revision the write produced until the server
 * snapshot catches up. */
export function useRevisionDraft<
  T extends Revisioned,
  V,
  R extends { revision?: number },
>({
  server,
  save,
  onSaved,
  load,
  reload,
}: {
  /** Latest server snapshot; absent while creating the resource. */
  server: T | undefined
  /** Writes the draft with `If-Match: revision` (absent while creating). */
  save: (values: V, revision: number | undefined) => Promise<R>
  onSaved?: (result: R, values: V) => void
  /** Replaces the draft's values with `resource`'s (discard, reload). */
  load: (resource: T | undefined) => void
  /** Re-reads the resource. */
  reload?: () => Promise<T | undefined>
}): RevisionDraft<T, V> {
  // Snapshot the dirty draft started from; absent while the draft is clean.
  const [pinned, setPinned] = useState<T | undefined>(undefined)
  // Last save, until the server snapshot reaches the revision it produced.
  const [saved, setSaved] = useState<{ revision: number; values: V }>()
  const [isReloading, setReloading] = useState(false)
  const base = pinned ?? server
  const ahead =
    saved && (!base || saved.revision > base.revision) ? saved : undefined
  const revision = ahead?.revision ?? base?.revision
  const mutation = useMutation({
    meta: { inlineError: true },
    mutationFn: (values: V) => save(values, revision),
    onSuccess: (result, values) => {
      setPinned(undefined)
      setSaved(
        result.revision === undefined
          ? undefined
          : { revision: result.revision, values }
      )
      onSaved?.(result, values)
    },
  })
  const track = useCallback(
    (dirty: boolean) =>
      setPinned((current) => (dirty ? (current ?? server) : undefined)),
    [server]
  )
  const restart = (resource: T | undefined) => {
    setPinned(undefined)
    mutation.reset()
    load(resource)
  }
  return {
    base,
    savedValues: ahead?.values,
    revision,
    stale:
      !!pinned &&
      !!server &&
      revision !== undefined &&
      server.revision > revision,
    conflict: isRevisionConflict(mutation.error),
    error: mutation.error,
    isSaving: mutation.isPending,
    track,
    save: (values) => mutation.mutate(values),
    saveAsync: (values) => mutation.mutateAsync(values),
    discard: () => restart(server),
    reload: async () => {
      setReloading(true)
      try {
        restart((await reload?.()) ?? server)
      } finally {
        setReloading(false)
      }
    },
    isReloading,
  }
}

/** Reports a React Hook Form's dirty state to its draft (`track`). */
export function useDraftForm(
  form: {
    subscribe: (payload: {
      formState: { isDirty: true }
      callback: (state: { isDirty?: boolean }) => void
    }) => () => void
  },
  track: (dirty: boolean) => void
) {
  useEffect(
    () =>
      form.subscribe({
        formState: { isDirty: true },
        callback: ({ isDirty }) => track(!!isDirty),
      }),
    [form, track]
  )
}
