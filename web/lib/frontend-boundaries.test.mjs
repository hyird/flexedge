import { fileURLToPath } from 'node:url'
import { ESLint } from 'eslint'
import { expect, test } from 'vitest'

const eslint = new ESLint({
  cwd: fileURLToPath(new URL('../../', import.meta.url)),
})

async function ruleIds(source, filePath) {
  const [result] = await eslint.lintText(source, { filePath })
  return result.messages.map((message) => message.ruleId)
}

test('the actual lint config rejects business dependencies in shared layers', async () => {
  for (const filePath of [
    'web/lib/boundary-probe.ts',
    'web/components/boundary-probe.tsx',
    'web/components/layout/boundary-probe.tsx',
    // No layout file is exempt (the task center lives in features/tasks).
    'web/components/layout/task-center.tsx',
  ]) {
    for (const module of [
      '@/features/tasks/types',
      '../../features/tasks/types',
      '@/routes/sign-in',
    ]) {
      expect(
        await ruleIds(`export { Example } from '${module}'`, filePath)
      ).toContain('no-restricted-imports')
    }
  }
}, 15000)

test('UI files remain linted and can reuse shared primitives', async () => {
  const [result] = await eslint.lintText("export { cn } from '@/lib/utils'", {
    filePath: 'web/components/boundary-probe.tsx',
  })
  expect(result.messages).toEqual([])
})

test('feature UI and hooks cannot import data access while data modules and error formatting remain available', async () => {
  for (const filePath of [
    'web/features/nodes/boundary-probe.tsx',
    'web/features/nodes/boundary-probe.ts',
  ]) {
    for (const source of [
      "import { getData as read } from '@/lib/api'; void read",
      "import { sendData } from '../../lib/api'; void sendData",
      "import { streamPath } from '@/lib/api'; void streamPath",
      "export { api } from '@/lib/api'",
      "import * as transport from '@/lib/api'; void transport",
      "import axios from 'axios'; void axios",
      "import { createApiClient } from '@/lib/api-client'; void createApiClient",
      "import { readLiveQuery } from '@/lib/live-query'; void readLiveQuery",
      "import { readLiveEvent } from '@/lib/live-query'; void readLiveEvent",
      "import { readLiveAppend } from '@/lib/live-query'; void readLiveAppend",
      "import { openEventStream } from '@/lib/event-stream'; void openEventStream",
      "import { resourceListSearchQuery } from '@/lib/resource'; void resourceListSearchQuery",
    ]) {
      expect(await ruleIds(source, filePath)).toContain('no-restricted-imports')
    }
  }
  for (const [filePath, source] of [
    [
      'web/features/nodes/boundary-probe.tsx',
      "import { apiErrorMessage } from '@/lib/api'; void apiErrorMessage",
    ],
    [
      'web/features/nodes/data.ts',
      "import { getData, streamPath } from '@/lib/api'; void getData; void streamPath",
    ],
    [
      'web/features/nodes/data.ts',
      "import { readLiveQuery } from '@/lib/live-query'; void readLiveQuery",
    ],
  ]) {
    const [result] = await eslint.lintText(source, { filePath })
    expect(result.messages).toEqual([])
  }
}, 15000)

test('business data never polls', async () => {
  for (const source of [
    'export const options = { refetchInterval: 1000 }',
    "export const options = { 'refetchInterval': 1000 }",
    'export const options = { refetchIntervalInBackground: true }',
  ]) {
    for (const filePath of [
      'web/features/nodes/data.ts',
      'web/components/boundary-probe.tsx',
    ]) {
      expect(await ruleIds(source, filePath)).toContain('no-restricted-syntax')
    }
  }
})
