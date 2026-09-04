import { fileURLToPath, URL } from 'node:url'
import { defineConfig, loadEnv } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { tanstackRouter } from '@tanstack/router-plugin/vite'
import { viteSingleFile } from 'vite-plugin-singlefile'

const rootDir = fileURLToPath(new URL('.', import.meta.url))

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, rootDir, '')
  const backendPort = env.PORT?.trim() || '1102'

  return {
    root: fileURLToPath(new URL('./web', import.meta.url)),
    envDir: rootDir,
    plugins: [
      tanstackRouter({
        target: 'react',
        autoCodeSplitting: true,
        routesDirectory: './routes',
        generatedRouteTree: './routeTree.gen.ts',
      }),
      react(),
      tailwindcss(),
      viteSingleFile(),
    ],
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
    },
  }
})
