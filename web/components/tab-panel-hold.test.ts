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

function setup() {
  const client = new QueryClient()
  clients.push(client)
  return { client, hold: new TabPanelHold(client.getQueryCache()) }
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

describe('tab panel hold', () => {
  it("keeps the outgoing height until the incoming tab's first data lands", async () => {
    const { client, hold } = setup()
    hold.hold(outgoing)
    expect(hold.getMinHeight()).toBe(900)

    // The incoming tab mounts and starts reading its snapshot.
    const incoming = startFirstLoad(client, 'incoming')
    hold.watch()
    expect(hold.getMinHeight()).toBe(900)

    await incoming.settle('data')
    // Only the height that keeps the scroll position stays reserved.
    expect(hold.getMinHeight()).toBe(700)
  })

  it('releases at once for a tab whose data is cached', () => {
    const { client, hold } = setup()
    client.setQueryData(['incoming'], 'cached')
    hold.hold(outgoing)
    hold.watch()
    expect(hold.getMinHeight()).toBe(700)
  })

  it('ignores loads that started before the switch', () => {
    const { client, hold } = setup()
    startFirstLoad(client, 'header')
    hold.hold(outgoing)
    hold.watch()
    expect(hold.getMinHeight()).toBe(700)
  })

  it('releases when the first load fails', async () => {
    const { client, hold } = setup()
    hold.hold(outgoing)
    const incoming = startFirstLoad(client, 'incoming')
    hold.watch()
    await incoming.settle('error')
    expect(hold.getMinHeight()).toBe(700)
  })

  it('keeps nothing once loaded when the tabs were scrolled out of view', async () => {
    const { client, hold } = setup()
    hold.hold({ ...outgoing, startsOnScreen: false })
    const incoming = startFirstLoad(client, 'incoming')
    hold.watch()
    expect(hold.getMinHeight()).toBe(900)
    await incoming.settle('data')
    expect(hold.getMinHeight()).toBeUndefined()
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
