import process from "node:process";

let chromium;
try {
  ({ chromium } = await import("playwright"));
} catch {
  console.error("Playwright is required for --render. Install it with `npm install playwright`.");
  process.exit(2);
}

const url = process.argv[2];
const timeoutMs = Number(process.argv[3] || "15000");
const userAgent = process.argv[4] || "pluck/0.1";

if (!url) {
  console.error("Missing URL");
  process.exit(2);
}

const browser = await chromium.launch({ headless: true });
try {
  const page = await browser.newPage({ userAgent });
  await page.goto(url, { waitUntil: "domcontentloaded", timeout: timeoutMs });
  try {
    await page.waitForLoadState("networkidle", { timeout: Math.min(timeoutMs, 3000) });
  } catch {}
  const html = await page.content();
  process.stdout.write(JSON.stringify({ finalUrl: page.url(), html }));
} finally {
  await browser.close();
}
