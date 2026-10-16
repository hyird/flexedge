import { createElement } from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import { ClientIpTooltip } from './client-ip-tooltip'

const label = '203.0.113.10'
const render = (props: Parameters<typeof ClientIpTooltip>[0]) =>
  renderToStaticMarkup(createElement(ClientIpTooltip, props))

describe('client IP tooltip', () => {
  it('keeps the client label visible and AS details out of the resting view', () => {
    const html = render({
      label,
      asn: 'AS4134',
      asName: 'Chinanet',
      location: '中国 · 浙江省 · 杭州市',
    })
    expect(html).toContain(`aria-label="${label}"`)
    expect(html).toContain('tabindex="0"')
    expect(html).toContain(`>${label}</div>`)
    expect(html).not.toContain('AS4134')
    expect(html).not.toContain('Chinanet')
    expect(html).not.toContain('杭州市')
  })

  it('also hides partial AS data until the tooltip opens', () => {
    for (const props of [{ asn: 'AS4134' }, { asName: 'Chinanet' }]) {
      const html = render({ label, ...props })
      expect(html).toContain('data-slot="tooltip-trigger"')
      expect(html).not.toContain('AS4134')
      expect(html).not.toContain('Chinanet')
    }
  })

  it('retains an explicitly supplied geographic tooltip without AS data', () => {
    const html = render({ label, asn: ' ', asName: '\t', location: '中国' })
    expect(html).toContain('data-slot="tooltip-trigger"')
    expect(html).toContain(`aria-label="${label}"`)
  })

  it('does not create an empty tooltip when no AS or geography is supplied', () => {
    const html = render({ label, asn: ' ', asName: '\t' })
    expect(html).toBe(`<span>${label}</span>`)
  })

  it('preserves custom visible content and truncation in both states', () => {
    for (const asn of [undefined, 'AS4134']) {
      const html = render({
        label,
        asn,
        className: 'block w-full truncate',
        children: createElement('span', {}, '中国 浙江 杭州'),
      })
      expect(html).toContain('block w-full truncate')
      expect(html).toContain('<span>中国 浙江 杭州</span>')
      expect(html).not.toContain('AS4134')
    }
  })
})
