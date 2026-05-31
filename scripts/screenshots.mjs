// Capture UI screenshots of the built static app for CI artifacts.
//
// Serves the production `dist/` via Vite's preview server (built with root base)
// and drives it with Playwright/Chromium at desktop and mobile viewports.
// Run in CI after: npm run build (VITE_STATIC=true) + playwright install chromium.

import { preview } from 'vite';
import { chromium } from 'playwright';
import { mkdir } from 'node:fs/promises';

const PORT = 4173;
const OUT = 'docs/screenshots';

async function main() {
  await mkdir(OUT, { recursive: true });
  const server = await preview({ preview: { port: PORT } });
  const base = `http://localhost:${PORT}`;
  const browser = await chromium.launch();

  try {
    // 1) Desktop — default landing search (Paris).
    const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
    await page.goto(base, { waitUntil: 'networkidle' });
    await page.waitForSelector('article', { timeout: 20000 });
    await page.screenshot({ path: `${OUT}/01-home-paris.png`, fullPage: true });

    // 2) Switch destination to Dubai and search again.
    await page.locator('select').first().selectOption('Dubai');
    await page.getByRole('button', { name: /find deals/i }).click();
    await page.waitForSelector('text=hotels in Dubai', { timeout: 20000 });
    await page.screenshot({ path: `${OUT}/02-dubai.png`, fullPage: true });

    // 3) Mobile viewport.
    const mobile = await browser.newPage({ viewport: { width: 390, height: 844 } });
    await mobile.goto(base, { waitUntil: 'networkidle' });
    await mobile.waitForSelector('article', { timeout: 20000 });
    await mobile.screenshot({ path: `${OUT}/03-mobile.png`, fullPage: true });

    console.log('Screenshots written to', OUT);
  } finally {
    await browser.close();
    server.httpServer.close();
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
