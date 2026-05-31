import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vite.dev/config/
// - In dev, proxy API calls to the Express server (npm run dev runs both).
// - For GitHub Pages, set VITE_BASE (e.g. "/full-stack-blog/") so asset URLs
//   resolve under the project subpath.
export default defineConfig({
  base: process.env.VITE_BASE || '/',
  plugins: [react()],
  server: {
    proxy: {
      '/api': {
        target: 'http://localhost:3001',
        changeOrigin: true,
      },
    },
  },
})
