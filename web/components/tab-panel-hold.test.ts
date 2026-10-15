import { createElement } from 'react'
import { notifyManager, QueryClient } from '@tanstack/react-query'
import { renderToStaticMarkup } from 'react-dom/server'
import { afterAll, afterEach, beforeAll, describe, expect, it } from 'vitest'
import { useTabSwitchGuard, type TabSwitchGuard } from '@/lib/unsaved-changes'
import { TabPanelHold } from './tab-panel-hold'

// Deliver Query's batched notifications synchronously instead of in a
// zero-delay timeout, so each settled load is observable right away.
beforeAll(() => notifyManager.setScheduler((callback) => callback()))
afterAll(() =>
  notifyManager.setScheduler((callback) => {
    setTimeout(callback, 0)
  })
)

const clients: QueryClient[] = []
afterEach(() => {
  clients.splice(0).forEach((client) => client.clear())
})

/** A hold whose owner "commits" like `useTabPanelHold`: every change it
 * publishes re-renders the owner, whose effect then calls `settle()`. */
function setup() {
  const client = new QueryClient()
  clients.push(client)
  const hold = new TabPanelHold(client.getQueryCache())
  let renders = 0
  hold.subscribe(() => renders++)
  return { client, hold, renders: () => renders }
}

/** Starts a first load (a snapshot or the first stream frame); `settle`
 * delivers it or fails it and waits until the query has settled. */
function startFirstLoad(client: QueryClient, key: string) {
  let deliver: (outcome: 'data' | 'error') => void = () => undefined
  const loaded = client
    .fetchQuery({
      queryKey: [key],
      // Runs synchronously inside `fetchQuery`, so `deliver` is set on return.
      queryFn: () =>
        new Promise<string>((resolve, reject) => {
          deliver = (outcome) =>
            outcome === 'data' ? resolve('frame') : reject(new Error('failed'))
        }),
    })
    .catch(() => undefined)
  return {
    settle: async (outcome: 'data' | 'error') => {
      deliver(outcome)
      await loaded
    },
  }
}

const outgoing = { height: 900, below: 200, startsOnScreen: true }
const held = { minHeight: 900, maxHeight: 900, overflow: 'clip' }

describe('tab panel hold', () => {
  it("keeps exactly the outgoing height until the incoming tab's first data lands", async () => {
    const { client, hold, renders } = setup()
    hold.hold(outgoing)
    expect(hold.getStyle()).toEqual(held)

    // The incoming tab mounts and starts reading its snapshot.
    const incoming = startFirstLoad(client, 'incoming')
    hold.settle()
    expect(hold.getStyle()).toEqual(held)

    const before = renders()
    await incoming.settle('data')
    // The data's commit re-renders the owner; its effect releases the hold.
    expect(renders()).toBeGreaterThan(before)
    expect(hold.getStyle()).toEqual(held)
    hold.settle()
    // Only the height that keeps the scroll position stays reserved.
    expect(hold.getStyle()).toEqual({ minHeight: 700 })
  })

  it('releases at once for a tab whose data is cached', () => {
    const { client, hold } = setup()
    client.setQueryData(['incoming'], 'cached')
    hold.hold(outgoing)
    hold.settle()
    expect(hold.getStyle()).toEqual({ minHeight: 700 })
  })

  it('ignores loads that started before the switch', () => {
    const { client, hold } = setup()
    startFirstLoad(client, 'header')
    const release = hold.addLoad()
    hold.hold(outgoing)
    hold.settle()
    expect(hold.getStyle()).toEqual({ minHeight: 700 })
    release()
  })

  it('releases when the first load fails', async () => {
    const { client, hold } = setup()
    hold.hold(outgoing)
    const incoming = startFirstLoad(client, 'incoming')
    hold.settle()
    await incoming.settle('error')
    hold.settle()
    expect(hold.getStyle()).toEqual({ minHeight: 700 })
  })

  it('keeps nothing once loaded when the tabs were scrolled out of view', async () => {
    const { client, hold } = setup()
    hold.hold({ ...outgoing, startsOnScreen: false })
    const incoming = startFirstLoad(client, 'incoming')
    hold.settle()
    expect(hold.getStyle()).toEqual(held)
    await incoming.settle('data')
    hold.settle()
    expect(hold.getStyle()).toBeUndefined()
  })

  it('waits for loads outside Query, such as a log tail before its first batch', () => {
    const { hold, renders } = setup()
    hold.hold(outgoing)
    // The incoming tab's layout effect registers its opening tail.
    const firstBatch = hold.addLoad()
    hold.settle()
    expect(hold.getStyle()).toEqual(held)

    const before = renders()
    firstBatch()
    expect(renders()).toBeGreaterThan(before)
    hold.settle()
    expect(hold.getStyle()).toEqual({ minHeight: 700 })
  })

  it('keeps holding for a load that starts in the commit where a query landed', async () => {
    const { client, hold } = setup()
    hold.hold(outgoing)
    // The logs tab first reads the header snapshot …
    const header = startFirstLoad(client, 'header-snapshot')
    hold.settle()
    await header.settle('data')
    // … and in the same commit opens its tail, before the owner's effect.
    const firstBatch = hold.addLoad()
    hold.settle()
    expect(hold.getStyle()).toEqual(held)

    firstBatch()
    hold.settle()
    expect(hold.getStyle()).toEqual({ minHeight: 700 })
  })

  it('runs the panel hold before switches requested outside the tab list', () => {
    let guard: TabSwitchGuard | undefined
    renderToStaticMarkup(
      createElement(function GuardProbe() {
        guard = useTabSwitchGuard()
        return null
      })
    )
    const calls: string[] = []
    guard!.beforeSwitchRef.current = () => calls.push('hold')
    guard!.request(() => calls.push('switch'))
    expect(calls).toEqual(['hold', 'switch'])
  })
})
