import js from '@eslint/js'
import pluginQuery from '@tanstack/eslint-plugin-query'
import reactHooks from 'eslint-plugin-react-hooks'
import reactRefresh from 'eslint-plugin-react-refresh'
import { defineConfig } from 'eslint/config'
import globals from 'globals'
import tseslint from 'typescript-eslint'

export default defineConfig(
  { ignores: ['build', 'node_modules'] },
  {
    extends: [
      js.configs.recommended,
      ...tseslint.configs.recommended,
      ...pluginQuery.configs['flat/recommended'],
    ],
    files: ['web/**/*.{ts,tsx}'],
    languageOptions: {
      ecmaVersion: 2022,
      globals: globals.browser,
    },
    plugins: {
      'react-hooks': reactHooks,
      'react-refresh': reactRefresh,
    },
    rules: {
      ...reactHooks.configs.recommended.rules,
      // React Hook Form intentionally exposes mutable APIs.
      'react-hooks/incompatible-library': 'off',
      'react-refresh/only-export-components': [
        'warn',
        { allowConstantExport: true },
      ],
      'no-console': 'error',
      'no-unused-vars': 'off',
      '@typescript-eslint/no-unused-vars': [
        'error',
        {
          argsIgnorePattern: '^_',
          caughtErrorsIgnorePattern: '^_',
          varsIgnorePattern: '^_',
          ignoreRestSiblings: true,
        },
      ],
      '@typescript-eslint/consistent-type-imports': [
        'error',
        {
          prefer: 'type-imports',
          fixStyle: 'inline-type-imports',
          disallowTypeAnnotations: false,
        },
      ],
      'no-duplicate-imports': 'error',
      // Business data never polls: live views use SSE (web/lib/live-query.ts).
      'no-restricted-syntax': [
        'error',
        {
          selector:
            'Identifier[name=/^refetchInterval/], Property[key.value=/^refetchInterval/]',
          message:
            'Polling is forbidden; live data comes from the SSE stream in the domain data module.',
        },
      ],
    },
  },
  {
    // Feature UI and hooks reach the API only through their domain data
    // module (`features/<domain>/data.ts`).
    files: ['web/features/**/*.{ts,tsx}'],
    ignores: ['web/features/**/data.ts', 'web/**/*.test.{ts,tsx}'],
    rules: {
      'no-restricted-imports': [
        'error',
        {
          paths: [
            {
              name: 'axios',
              message:
                'UI must use domain data functions instead of HTTP clients.',
            },
          ],
          patterns: [
            {
              group: ['@/lib/api', '**/lib/api', '**/lib/api.ts'],
              importNames: [
                'api',
                'getData',
                'sendData',
                'streamPath',
                'normalizeApiPath',
              ],
              message:
                'HTTP requests belong in domain data modules; UI may import error formatting helpers.',
            },
            {
              group: [
                '@/lib/api-client',
                '**/lib/api-client',
                '**/lib/api-client.ts',
              ],
              message: 'UI must not construct HTTP clients.',
            },
            {
              group: [
                '@/lib/live-query',
                '**/lib/live-query',
                '**/lib/live-query.ts',
              ],
              importNames: [
                'LiveQueryManager',
                'readLiveQuery',
                'readLiveEvent',
                'readLiveAppend',
              ],
              message: 'Live reads belong in domain data modules.',
            },
            {
              group: [
                '@/lib/event-stream',
                '**/lib/event-stream',
                '**/lib/event-stream.ts',
              ],
              message: 'Streams are opened by the live-query layer only.',
            },
            {
              group: [
                '@/lib/resource',
                '**/lib/resource',
                '**/lib/resource.ts',
              ],
              importNames: ['resourceListSearchQuery'],
              message: 'Resource searches belong in domain data modules.',
            },
          ],
        },
      ],
    },
  },
  {
    // Shared primitives, utilities and the app layout cannot depend on
    // business features; routes assemble them.
    files: ['web/lib/**/*.{ts,tsx}', 'web/components/**/*.{ts,tsx}'],
    languageOptions: { parser: tseslint.parser },
    rules: {
      'no-restricted-imports': [
        'error',
        {
          patterns: [
            {
              group: [
                '@/features',
                '@/features/**',
                '**/features/**',
                '@/routes',
                '@/routes/**',
                '**/routes/**',
              ],
              message:
                'Shared primitives and utilities must not import business features or routes.',
            },
          ],
        },
      ],
    },
  }
)
