import process from "node:process";

let JSDOM;
let Readability;
try {
  ({ JSDOM } = await import("jsdom"));
  ({ Readability } = await import("@mozilla/readability"));
} catch {
  console.error("Install `jsdom` and `@mozilla/readability` to use this benchmark baseline.");
  process.exit(2);
}

const url = process.argv[2];
if (!url) {
  console.error("Missing URL");
  process.exit(2);
}

const response = await fetch(url, {
  headers: {
    "user-agent": "pluck-benchmark/0.1",
  },
});

const html = await response.text();
const dom = new JSDOM(html, { url: response.url });
const article = new Readability(dom.window.document).parse();
process.stdout.write((article?.textContent || "").trim() + "\n");
