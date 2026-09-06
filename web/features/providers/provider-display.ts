export function providerLabel(provider: string) {
  if (provider === 'cloudflare') return 'Cloudflare'
  if (provider === 'aliyun') return '阿里云'
  if (provider === 'letsencrypt') return "Let's Encrypt"
  if (provider === 'zerossl') return 'ZeroSSL'
  return provider
}

