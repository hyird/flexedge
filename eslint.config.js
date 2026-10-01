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
    },
  },
  {
    files: ['web/features/**/*.tsx'],
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
              importNames: ['api', 'getData', 'getAllPages', 'sendData'],
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
          ],
        },
      ],
    },
  },
  {
    // Shared primitives and utilities cannot depend on business features.
    files: ['web/lib/**/*.{ts,tsx}', 'web/components/*.{ts,tsx}'],
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
