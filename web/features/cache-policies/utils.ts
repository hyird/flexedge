import type { CacheRule } from './types'

export const DEFAULT_CACHE_RULE_ID = '00000000-0000-4000-8000-000000000000'

export function isDefaultCacheRule(rule: Pick<CacheRule, 'id'>) {
  return rule.id.toLowerCase() === DEFAULT_CACHE_RULE_ID
}

export const createDefaultCacheRule = (): CacheRule => ({
  id: DEFAULT_CACHE_RULE_ID,
  name: '默认规则',
  status: 'enabled',
  match_type: 'all',
  patterns: [],
  action: 'cache',
  ttl_seconds: 0,
  query_mode: 'include',
  status_codes: [200, 301, 302, 404],
  min_object_bytes: 0,
  max_object_bytes: 0,
  stale_if_error_seconds: 0,
  ignore_origin_cache_control: false,
  range_enabled: true,
})

const STATIC_FILE_EXTENSIONS = [
  '.7z',
  '.aac',
  '.apk',
  '.avif',
  '.bin',
  '.bmp',
  '.bz2',
  '.cjs',
  '.css',
  '.dmg',
  '.doc',
  '.docx',
  '.eot',
  '.flac',
  '.gif',
  '.gz',
  '.ico',
  '.iso',
  '.jar',
  '.jpeg',
  '.jpg',
  '.js',
  '.m4a',
  '.mid',
  '.midi',
  '.mjs',
  '.mov',
  '.mp3',
  '.mp4',
  '.oga',
  '.ogg',
  '.ogv',
  '.otf',
  '.pdf',
  '.ppt',
  '.pptx',
  '.png',
  '.rar',
  '.svg',
  '.tar',
  '.tgz',
  '.tif',
  '.tiff',
  '.ttf',
  '.wav',
  '.wasm',
  '.webm',
  '.webp',
  '.woff',
  '.woff2',
  '.xls',
  '.xlsx',
  '.xz',
  '.zip',
] as const

export const createCacheRule = (
  action: CacheRule['action'] = 'cache'
): CacheRule => ({
  id: crypto.randomUUID(),
  name: action === 'cache' ? '静态文件' : '绕过条件',
  status: 'enabled',
  match_type: action === 'cache' ? 'extension' : 'all',
  patterns: action === 'cache' ? [...STATIC_FILE_EXTENSIONS] : [],
  action,
  ttl_seconds: action === 'cache' ? 3600 : 0,
  query_mode: 'include',
  status_codes: [200],
  min_object_bytes: 0,
  max_object_bytes: 0,
  stale_if_error_seconds: 0,
  ignore_origin_cache_control: false,
  range_enabled: true,
})

export const cloneCacheRule = (rule: CacheRule): CacheRule => ({
  ...rule,
  patterns: [...rule.patterns],
  status_codes: [...rule.status_codes],
})
