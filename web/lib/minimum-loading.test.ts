import { afterEach, beforeEach, expect, test, vi } from 'vitest'
import {
  LoadingEpoch,
  MinimumLoading,
  minimumLoadingMs,
} from './minimum-loading'

beforeEach(() => {
  vi.useFakeTimers()
})
afterEach(() => {
  vi.useRealTimers()
})

test('a first load answered early stays loading for the full minimum', () => {
  const loading = new MinimumLoading()
  const owner = {}
  const shown: string[] = []
  loading.start(owner)
  vi.advanceTimersByTime(30)
  loading.reveal(owner, () => shown.push('data'))
  vi.advanceTimersByTime(minimumLoadingMs - 31)
  expect(shown).toEqual([])
  vi.advanceTimersByTime(1)
  expect(shown).toEqual(['data'])
  expect(vi.getTimerCount()).toBe(0)
})

test('only the newest outcome of a held first load is shown, once', () => {
  const loading = new MinimumLoading()
  const owner = {}
  const shown: string[] = []
  loading.start(owner)
  loading.reveal(owner, () => shown.push('error'))
  vi.advanceTimersByTime(100)
  loading.reveal(owner, () => shown.push('frame 1'))
  loading.reveal(owner, () => shown.push('frame 2'))
  vi.advanceTimersByTime(minimumLoadingMs)
  expect(shown).toEqual(['frame 2'])
})

test('cached data, live updates and refetches reveal without delay', () => {
  const loading = new MinimumLoading()
  const owner = {}
  const shown: string[] = []
  // Never started: the data was cached.
  loading.reveal(owner, () => shown.push('cached'))
  expect(shown).toEqual(['cached'])
  loading.start(owner)
  loading.reveal(owner, () => shown.push('first'))
  vi.advanceTimersByTime(minimumLoadingMs)
  // The hold ended with the first outcome; later frames show at once.
  loading.reveal(owner, () => shown.push('live'))
  expect(shown).toEqual(['cached', 'first', 'live'])
  expect(vi.getTimerCount()).toBe(0)
})

test('a slow first load reveals as soon as it arrives', () => {
  const loading = new MinimumLoading()
  const owner = {}
  const shown: string[] = []
  loading.start(owner)
  vi.advanceTimersByTime(800)
  loading.reveal(owner, () => shown.push('data'))
  expect(shown).toEqual(['data'])
  expect(vi.getTimerCount()).toBe(0)
})

test('a loading state already shown long enough is not held again', () => {
  const loading = new MinimumLoading()
  const owner = {}
  const shown: string[] = []
  const shownAt = Date.now()
  vi.advanceTimersByTime(300)
  // Shown since 300 ms ago: 200 ms remain.
  loading.start(owner, shownAt)
  loading.reveal(owner, () => shown.push('data'))
  vi.advanceTimersByTime(199)
  expect(shown).toEqual([])
  vi.advanceTimersByTime(1)
  expect(shown).toEqual(['data'])
  vi.advanceTimersByTime(minimumLoadingMs)
  loading.start(owner, shownAt)
  loading.reveal(owner, () => shown.push('again'))
  expect(shown).toEqual(['data', 'again'])
})

test('cancel and clear drop waiting outcomes and their timer', () => {
  const loading = new MinimumLoading()
  const shown: string[] = []
  const first = {}
  const second = {}
  loading.start(first)
  loading.start(second)
  loading.reveal(first, () => shown.push('first'))
  loading.reveal(second, () => shown.push('second'))
  expect(vi.getTimerCount()).toBe(1)
  loading.cancel(first)
  expect(vi.getTimerCount()).toBe(1)
  loading.clear()
  expect(vi.getTimerCount()).toBe(0)
  vi.advanceTimersByTime(minimumLoadingMs * 2)
  expect(shown).toEqual([])
})

test('loads that started within one frame reveal together in one batch', () => {
  const batches: string[][] = []
  let current: string[] | null = null
  const loading = new MinimumLoading(minimumLoadingMs, (reveal) => {
    current = []
    reveal()
    batches.push(current)
    current = null
  })
  const header = {}
  const table = {}
  const later = {}
  loading.start(header)
  vi.advanceTimersByTime(8)
  loading.start(table)
  vi.advanceTimersByTime(100)
  loading.start(later)
  loading.reveal(header, () => current?.push('header'))
  loading.reveal(table, () => current?.push('table'))
  loading.reveal(later, () => current?.push('later'))
  // The header's minimum passed, but the table started 8 ms later: both wait
  // for the table and render together.
  vi.advanceTimersByTime(minimumLoadingMs - 108)
  expect(batches).toEqual([])
  vi.advanceTimersByTime(8)
  expect(batches).toEqual([['header', 'table']])
  vi.advanceTimersByTime(99)
  expect(batches).toHaveLength(1)
  vi.advanceTimersByTime(1)
  expect(batches).toEqual([['header', 'table'], ['later']])
})

test('a zero minimum never holds', () => {
  const loading = new MinimumLoading(0)
  const owner = {}
  const shown: string[] = []
  loading.start(owner)
  loading.reveal(owner, () => shown.push('data'))
  expect(shown).toEqual(['data'])
})

test('a later step of the same loading sequence is not held another minimum', () => {
  const epoch = new LoadingEpoch()
  const loading = new MinimumLoading(minimumLoadingMs, undefined, epoch)
  const shown: string[] = []
  const header = {}
  const list = {}
  // The user switches; the header's skeleton mounts 40 ms later.
  epoch.begin()
  vi.advanceTimersByTime(40)
  loading.start(header)
  vi.advanceTimersByTime(40)
  loading.reveal(header, () => shown.push('header'))
  vi.advanceTimersByTime(minimumLoadingMs - 81)
  expect(shown).toEqual([])
  // Counted from the switch, not from the skeleton.
  vi.advanceTimersByTime(1)
  expect(shown).toEqual(['header'])
  // The list mounts once the header revealed and answers 60 ms later.
  vi.advanceTimersByTime(20)
  loading.start(list)
  vi.advanceTimersByTime(60)
  loading.reveal(list, () => shown.push('list'))
  expect(shown).toEqual(['header', 'list'])
  expect(vi.getTimerCount()).toBe(0)
})

test('a new navigation, or a load after the sequence went quiet, starts a new epoch', () => {
  const epoch = new LoadingEpoch()
  const loading = new MinimumLoading<string>(minimumLoadingMs, undefined, epoch)
  const shown: string[] = []
  epoch.begin()
  loading.start('page a')
  loading.reveal('page a', () => shown.push('page a'))
  vi.advanceTimersByTime(1000)
  expect(shown).toEqual(['page a'])

  // The next navigation counts from its own start.
  epoch.begin()
  vi.advanceTimersByTime(50)
  loading.start('page b')
  loading.reveal('page b', () => shown.push('page b'))
  vi.advanceTimersByTime(minimumLoadingMs - 51)
  expect(shown).toEqual(['page a'])
  vi.advanceTimersByTime(1)
  expect(shown).toEqual(['page a', 'page b'])

  // A sheet opened 2 s later without a navigation keeps its own minimum.
  vi.advanceTimersByTime(2000)
  loading.start('sheet')
  vi.advanceTimersByTime(10)
  loading.reveal('sheet', () => shown.push('sheet'))
  vi.advanceTimersByTime(minimumLoadingMs - 11)
  expect(shown).toEqual(['page a', 'page b'])
  vi.advanceTimersByTime(1)
  expect(shown).toEqual(['page a', 'page b', 'sheet'])
})
