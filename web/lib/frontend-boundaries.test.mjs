import { fileURLToPath } from 'node:url'
import { ESLint } from 'eslint'
import { expect, test } from 'vitest'

const eslint = new ESLint({
  cwd: fileURLToPath(new URL('../../', import.meta.url)),
})

test('the actual lint config rejects business dependencies in shared layers', async () => {
  for (const filePath of [
    'web/lib/boundary-probe.ts',
    'web/components/ui/boundary-probe.tsx',
    'web/components/data-table/boundary-probe.tsx',
  ]) {
    for (const module of [
      '@/features/tasks/types',
      '../../features/tasks/types',
      '@/routes/sign-in',
    ]) {
      const [result] = await eslint.lintText(
        `export { Example } from '${module}'`,
        { filePath }
      )
      expect(
        result.messages.some(
          (message) => message.ruleId === 'no-restricted-imports'
        )
      ).toBe(true)
    }
  }
})

test('UI files remain linted and can reuse shared primitives', async () => {
  const [result] = await eslint.lintText("export { cn } from '@/lib/utils'", {
    filePath: 'web/components/ui/boundary-probe.tsx',
  })
  expect(result.messages).toEqual([])
})

test('feature UI cannot import HTTP operations while data modules and error formatting remain available', async () => {
  for (const source of [
    "import { getData as read } from '@/lib/api'; void read",
    "import { sendData } from '../../lib/api'; void sendData",
    "export { api } from '@/lib/api'",
    "import * as transport from '@/lib/api'; void transport",
    "import axios from 'axios'; void axios",
    "import { createApiClient } from '@/lib/api-client'; void createApiClient",
  ]) {
    const [result] = await eslint.lintText(source, {
      filePath: 'web/features/nodes/boundary-probe.tsx',
    })
    expect(
      result.messages.some(
        (message) => message.ruleId === 'no-restricted-imports'
      )
    ).toBe(true)
  }
  for (const [filePath, source] of [
    [
      'web/features/nodes/boundary-probe.tsx',
      "import { apiErrorMessage } from '@/lib/api'; void apiErrorMessage",
    ],
    [
      'web/features/nodes/data.ts',
      "import { getData } from '@/lib/api'; void getData",
    ],
  ]) {
    const [result] = await eslint.lintText(source, { filePath })
    expect(result.messages).toEqual([])
  }
})
