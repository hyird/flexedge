/** Mirror of the backend `service::common::certificate_covers_hostname`:
 * names compare case-insensitively without trailing dots, a wildcard name
 * `*.parent` covers exactly one extra left-most label, and a wildcard
 * hostname is only covered by the identical wildcard name. */
function normalizeDomainName(value: string) {
  return value.replace(/\.+$/, '').toLowerCase()
}

export function certificateCoversHostname(
  certificateName: string,
  hostname: string
) {
  const name = normalizeDomainName(certificateName)
  const host = normalizeDomainName(hostname)
  if (!name || !host) return false
  if (name === host) return true
  if (!name.startsWith('*.') || host.startsWith('*.')) return false
  const suffix = name.slice(1)
  if (host.length <= suffix.length || !host.endsWith(suffix)) return false
  return !host.slice(0, host.length - suffix.length).includes('.')
}

/** Whether any certificate name covers at least one of the hostnames. */
export function certificateMatchesHostnames(
  certificateNames: readonly string[],
  hostnames: readonly string[]
) {
  return certificateNames.some((name) =>
    hostnames.some((hostname) => certificateCoversHostname(name, hostname))
  )
}

/** Hostnames that none of the certificates cover. */
export function uncoveredHostnames(
  certificates: readonly { domains: readonly string[] }[],
  hostnames: readonly string[]
) {
  return hostnames.filter(
    (hostname) =>
      !certificates.some((certificate) =>
        certificate.domains.some((name) =>
          certificateCoversHostname(name, hostname)
        )
      )
  )
}
