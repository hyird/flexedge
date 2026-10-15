import { createElement } from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import { AccessLogAsnDetail, AccessLogClientOrigin } from './access-log-asn'
import { accessLogAsnLabel } from './access-log-filter'

const chinanet = { client_ip_asn: 'AS4134', client_ip_as_name: 'Chinanet' }
const longName = {
  client_ip_asn: 'AS4294967295',
  client_ip_as_name: 'Very Long Autonomous System Name '.repeat(8).trim(),
}
const origin = (log: Parameters<typeof AccessLogClientOrigin>[0]['log']) =>
  renderToStaticMarkup(createElement(AccessLogClientOrigin, { log }))

describe('access log ASN', () => {
  it('labels the AS number and name and stays empty without AS data', () => {
    expect(accessLogAsnLabel(chinanet)).toBe('AS4134 Chinanet')
    expect(accessLogAsnLabel({ client_ip_asn: 'AS4134' })).toBe('AS4134')
    expect(accessLogAsnLabel({ client_ip_as_name: 'Chinanet' })).toBe(
      'Chinanet'
    )
    expect(accessLogAsnLabel({})).toBe('')
  })

  it('shows location and ASN on one truncated line with the full text as accessible name', () => {
    const html = origin({ client_ip_location: '台湾', ...chinanet })
    expect(html).toContain('aria-label="台湾 · AS4134 Chinanet"')
    expect(html).toContain('truncate')
    expect(html).toContain('台湾 · <span')
    expect(html).toContain('>AS4134</span> Chinanet')

    const long = origin({ client_ip_location: '未知地区', ...longName })
    expect(long).toContain(
      `aria-label="未知地区 · ${accessLogAsnLabel(longName)}"`
    )
    expect(long).toContain('truncate')
  })

  it('shows only the part that exists', () => {
    const asnOnly = origin(chinanet)
    expect(asnOnly).toContain('aria-label="AS4134 Chinanet"')
    expect(asnOnly).not.toContain(' · ')
    const locationOnly = origin({ client_ip_location: '台湾' })
    expect(locationOnly).toContain('aria-label="台湾"')
    expect(locationOnly).not.toContain('AS')
  })

  it('renders a dash when location and ASN are both absent', () => {
    expect(origin({})).toBe('<span class="text-xs text-muted">—</span>')
    expect(
      renderToStaticMarkup(createElement(AccessLogAsnDetail, { log: {} }))
    ).toBe('—')
  })

  it('shows the full name in the request detail', () => {
    const html = renderToStaticMarkup(
      createElement(AccessLogAsnDetail, { log: longName })
    )
    expect(html).toContain('break-words')
    expect(html).toContain('>AS4294967295</span> ')
    expect(html).toContain(longName.client_ip_as_name)
  })
})
