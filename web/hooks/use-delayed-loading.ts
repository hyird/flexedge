import { useEffect, useState } from 'react'

export type DelayedLoadingOptions = {
  delayMs?: number
  minVisibleMs?: number
}

type Listener = (visible: boolean) => void

/**
 * Keeps transient loading states out of the UI while guaranteeing a stable
 * skeleton once it has become visible.
 */
export class DelayedLoadingController {
  private loading = false
  private visible = false
  private shownAt = 0
  private timer: ReturnType<typeof setTimeout> | undefined

  constructor(
    private readonly listener: Listener,
    private readonly delayMs = 150,
    private readonly minVisibleMs = 300
  ) {}

  setLoading(loading: boolean) {
    this.loading = loading
    this.clearTimer()

    if (loading) {
      if (this.visible) return
      this.timer = setTimeout(() => {
        this.timer = undefined
        if (!this.loading) return
        this.visible = true
        this.shownAt = Date.now()
        this.listener(true)
      }, this.delayMs)
      return
    }

    if (!this.visible) return
    const remaining = Math.max(
      0,
      this.minVisibleMs - (Date.now() - this.shownAt)
    )
    this.timer = setTimeout(() => {
      this.timer = undefined
      if (this.loading) return
      this.visible = false
      this.listener(false)
    }, remaining)
  }

  dispose() {
    this.clearTimer()
    this.loading = false
    this.visible = false
  }

  isVisible() {
    return this.visible
  }

  private clearTimer() {
    if (this.timer !== undefined) {
      clearTimeout(this.timer)
      this.timer = undefined
    }
  }
}

export type DelayedLoadingState = {
  pending: boolean
  showSkeleton: boolean
}

export function useDelayedLoading(
  loading: boolean,
  { delayMs = 150, minVisibleMs = 300 }: DelayedLoadingOptions = {}
) {
  const [showSkeleton, setShowSkeleton] = useState(false)
  const [controller] = useState(
    () => new DelayedLoadingController(setShowSkeleton, delayMs, minVisibleMs)
  )

  useEffect(() => {
    controller.setLoading(loading)
  }, [controller, loading])

  useEffect(() => {
    return () => controller.dispose()
  }, [controller])

  return { pending: loading || showSkeleton, showSkeleton }
}
