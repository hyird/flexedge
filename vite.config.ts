import { parse } from 'node:path'
import { fileURLToPath, URL } from 'node:url'
import { defineConfig, loadEnv } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { getConfig } from '@tanstack/router-generator'
import { tanstackRouter } from '@tanstack/router-plugin/vite'

const rootDir = fileURLToPath(new URL('.', import.meta.url))

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, rootDir, '')
  const backendPort = env.PORT?.trim() || '1102'

  return {
    root: fileURLToPath(new URL('./web', import.meta.url)),
    envDir: rootDir,
    cacheDir: fileURLToPath(new URL('./build/cache/vite', import.meta.url)),
    plugins: [tanstackRouter(getConfig({}, rootDir)), react(), tailwindcss()],
    resolve: {
      alias: {
        '@': fileURLToPath(new URL('./web', import.meta.url)),
      },
    },
    server: {
      host: '0.0.0.0',
      port: 5173,
      proxy: {
        '/api': {
          target: `http://127.0.0.1:${backendPort}`,
          changeOrigin: true,
        },
      },
    },
    build: {
      outDir: '../build/web',
      emptyOutDir: true,
      rolldownOptions: {
        output: {
          hashCharacters: 'hex',
          entryFileNames: (chunk) =>
            `assets/${chunk.name.toLowerCase()}-[hash:12].js`,
          chunkFileNames: (chunk) =>
            `assets/${chunk.name.toLowerCase()}-[hash:12].js`,
          assetFileNames: (asset) => {
            const { name, ext } = parse(asset.names[0] ?? 'asset')
            return `assets/${name.toLowerCase()}-[hash:12]${ext.toLowerCase()}`
          },
        },
      },
    },
  }
})
