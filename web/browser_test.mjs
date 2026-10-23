// Optional browser harness. Uses an existing Playwright; installs nothing.
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {createServer} from 'node:http';
import {readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
const require = createRequire(import.meta.url);
const {chromium} = require(process.env.NTRK_PLAYWRIGHT || 'playwright');
const root = fileURLToPath(new URL('../out/web/', import.meta.url));
const mime = {'.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.wasm': 'application/wasm'};
const server = createServer(async (req, res) => {
  try {
    const name = path.basename(new URL(req.url, 'http://localhost').pathname) || 'index.html';
    const bytes = await readFile(path.join(root, name));
    res.writeHead(200, {'Content-Type': mime[path.extname(name)] || 'application/octet-stream'}).end(bytes);
  } catch { res.writeHead(404).end(); }
});
let browser;
try {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  browser = await chromium.launch({headless: true});
  const page = await browser.newPage({acceptDownloads: true});
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.addInitScript(() => {
    const Original = window.AudioWorkletNode;
    window.AudioWorkletNode = class extends Original {
      constructor(...args) { super(...args); window.testNode = this; window.testContext = args[0]; }
    };
  });
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  await page.waitForFunction(() => !document.getElementById('play').disabled);
  assert.match(await page.locator('#status').innerText(), /Ready/);
  assert.equal(await page.locator('tbody tr').count(), 32);
  assert.equal(await page.locator('thead button').count(), 16);
  assert.equal(await page.evaluate(() => testContext.state), 'suspended');
  await page.locator('#play').click();
  await page.waitForFunction(() => document.getElementById('position').textContent.startsWith('PLAY'));
  const peak = await page.evaluate(async () => {
    const analyser = testContext.createAnalyser(); analyser.fftSize = 2048; testNode.connect(analyser);
    const data = new Float32Array(2048); let peak = 0;
    for (let i = 0; i < 20; ++i) {
      await new Promise(resolve => setTimeout(resolve, 25)); analyser.getFloatTimeDomainData(data);
      for (const x of data) { if (!Number.isFinite(x)) throw new Error('Non-finite audio'); peak = Math.max(peak, Math.abs(x)); }
    }
    testNode.disconnect(analyser); return peak;
  });
  assert(peak > 0.001 && peak <= 1, `actual worklet audio peak ${peak}`);
  await page.locator('#pause').click();
  await page.waitForFunction(() => document.getElementById('position').textContent.startsWith('IDLE'));
  const paused = await page.locator('#position').innerText();
  await page.waitForTimeout(150);
  assert.equal(await page.locator('#position').innerText(), paused);
  await page.locator('thead button').first().click();
  await page.waitForFunction(() => document.querySelector('thead button').getAttribute('aria-pressed') === 'true');
  await page.locator('#stop').click();
  await page.waitForFunction(() => document.getElementById('position').textContent.includes('ORDER 00 / ROW 00'));
  assert.match(await page.locator('#position').innerText(), /ORDER 00 \/ ROW 00/);
  assert.match(await page.locator('thead button').first().innerText(), /MUTE/);
  await page.locator('#orders button').nth(1).click();
  await page.waitForFunction(() => document.getElementById('position').textContent.includes('ORDER 01'));
  assert.equal(await page.locator('#follow').isChecked(), false);
  await page.locator('#edit').check();
  const firstNote = '[data-row="0"][data-channel="0"][data-field="0"]';
  await page.locator(firstNote).click();
  await page.keyboard.press('x');
  await page.waitForFunction(sel => document.querySelector(sel).textContent === 'D-2', firstNote);
  await page.locator('[data-row="1"][data-channel="0"][data-field="3"]').click();
  await page.keyboard.type('7f');
  await page.waitForFunction(() => document.querySelector('[data-row="1"][data-channel="0"][data-field="3"]').textContent === '7F');
  await page.locator('[data-row="2"][data-channel="0"][data-field="0"]').click();
  await page.keyboard.press('Enter');
  await page.waitForFunction(() => document.querySelector('[data-row="2"][data-channel="0"][data-field="0"]').textContent === '^^^');
  assert.match(await page.locator('#dirty').innerText(), /UNSAVED/);
  const downloadReady = page.waitForEvent('download');
  await page.locator('#save').click();
  const download = await downloadReady, downloaded = await download.path();
  assert(download.suggestedFilename().endsWith('.ntrk'));
  const bytes = await readFile(downloaded); assert.equal(bytes.subarray(0, 4).toString(), 'NTRK');
  await page.locator('#open').setInputFiles({name: 'edited.ntrk', mimeType: 'application/octet-stream', buffer: bytes});
  await page.waitForFunction(() => document.getElementById('filename').textContent === 'edited.ntrk');
  await page.locator('#orders button').nth(1).click();
  await page.waitForFunction(() => document.getElementById('position').textContent.includes('ORDER 01'));
  assert.equal(await page.locator(firstNote).innerText(), 'D-2');
  assert.equal(await page.locator('[data-row="1"][data-channel="0"][data-field="3"]').innerText(), '7F');
  assert.equal(await page.locator('[data-row="2"][data-channel="0"][data-field="0"]').innerText(), '^^^');
  await page.locator('#open').setInputFiles({name: 'bad.ntrk', mimeType: 'application/octet-stream', buffer: Buffer.from('bad')});
  await page.waitForFunction(() => document.getElementById('status').textContent.includes('Invalid'));
  assert.equal(await page.locator('#filename').innerText(), 'edited.ntrk');
  assert.equal(await page.locator(firstNote).innerText(), 'D-2');
  if (process.env.NTRK_SCREENSHOT) await page.screenshot({path: process.env.NTRK_SCREENSHOT + '.desktop.png', fullPage: true});
  await page.setViewportSize({width: 390, height: 844});
  assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
  if (process.env.NTRK_SCREENSHOT) await page.screenshot({path: process.env.NTRK_SCREENSHOT, fullPage: true});
  await page.evaluate(() => testContext.close());
  assert.deepEqual(errors, []);
  console.log(`browser: suspended startup, gesture playback (peak ${peak.toFixed(3)}), pause/stop, mute, order, note/hex/off editing, download/reopen, invalid file, mobile layout passed`);
} finally {
  if (browser) await browser.close();
  await new Promise(resolve => server.close(resolve));
}
