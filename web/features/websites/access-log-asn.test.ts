import { createElement } from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import { AccessLogAsnCell, AccessLogAsnDetail } from './access-log-asn'
import { accessLogAsnLabel } from './access-log-filter'

const chinanet = { client_ip_asn: 'AS4134', client_ip_as_name: 'Chinanet' }
const longName = {
  client_ip_asn: 'AS4294967295',
  client_ip_as_name: 'Very Long Autonomous System Name '.repeat(8).trim(),
}
const cell = (log: Parameters<typeof AccessLogAsnCell>[0]['log']) =>
  renderToStaticMarkup(createElement(AccessLogAsnCell, { log }))

describe('access log ASN', () => {
  it('labels the AS number and name and stays empty without AS data', () => {
    expect(accessLogAsnLabel(chinanet)).toBe('AS4134 Chinanet')
    expect(accessLogAsnLabel({ client_ip_asn: 'AS4134' })).toBe('AS4134')
    expect(accessLogAsnLabel({ client_ip_as_name: 'Chinanet' })).toBe(
      'Chinanet'
    )
    expect(accessLogAsnLabel({})).toBe('')
  })

  it('renders the cell as one normal-size line with the AS number in mono', () => {
    const html = cell(chinanet)
    expect(html).toContain('aria-label="AS4134 Chinanet"')
    expect(html).toContain('truncate')
    expect(html).toContain('font-mono')
    expect(html).toContain('>AS4134</span> Chinanet')
    expect(html).not.toContain('text-xs')
    expect(html).not.toContain('<br')
  })

  it('truncates a long AS name with the full text as tooltip trigger label', () => {
    const html = cell(longName)
    expect(html).toContain(`aria-label="${accessLogAsnLabel(longName)}"`)
    expect(html).toContain('truncate')
    expect(html).toContain(longName.client_ip_as_name)
  })

  it('shows only the part that exists', () => {
    expect(cell({ client_ip_asn: 'AS4134' })).toContain('aria-label="AS4134"')
    const nameOnly = cell({ client_ip_as_name: 'Chinanet' })
    expect(nameOnly).toContain('aria-label="Chinanet"')
    expect(nameOnly).not.toContain('font-mono')
  })

  it('renders a dash without AS data', () => {
    expect(cell({})).toBe('<span class="text-muted">—</span>')
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
