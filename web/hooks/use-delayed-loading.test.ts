import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { DelayedLoadingController } from './use-delayed-loading'

describe('DelayedLoadingController', () => {
  beforeEach(() => vi.useFakeTimers())
  afterEach(() => vi.useRealTimers())

  it('delays short requests and cancels the pending skeleton', () => {
    const changes: boolean[] = []
    const controller = new DelayedLoadingController((value) =>
      changes.push(value)
    )

    controller.setLoading(true)
    vi.advanceTimersByTime(149)
    expect(changes).toEqual([])
    controller.setLoading(false)
    vi.advanceTimersByTime(500)
    expect(changes).toEqual([])
  })

  it('keeps a displayed skeleton visible for at least the minimum duration', () => {
    const changes: boolean[] = []
    const controller = new DelayedLoadingController((value) =>
      changes.push(value)
    )

    controller.setLoading(true)
    vi.advanceTimersByTime(150)
    expect(changes).toEqual([true])
    controller.setLoading(false)
    vi.advanceTimersByTime(299)
    expect(changes).toEqual([true])
    vi.advanceTimersByTime(1)
    expect(changes).toEqual([true, false])
  })

  it('cancels stale timers when loading is replaced or disposed', () => {
    const changes: boolean[] = []
    const controller = new DelayedLoadingController((value) =>
      changes.push(value)
    )

    controller.setLoading(true)
    vi.advanceTimersByTime(150)
    controller.setLoading(false)
    vi.advanceTimersByTime(100)
    controller.setLoading(true)
    vi.advanceTimersByTime(500)
    expect(changes).toEqual([true])
    controller.dispose()
    vi.advanceTimersByTime(500)
    expect(changes).toEqual([true])
  })
})
