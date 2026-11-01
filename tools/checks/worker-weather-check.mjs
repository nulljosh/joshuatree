#!/usr/bin/env node
// 7.6.0: the Worker's /api/weather, which the Pi's Weather fetches as it opens (arch/arm64/feeds.c). With Open-Meteo
// mocked, the reply must be the WEATHER.TXT lines user/weather_pi.c reads: an ASCII city from request.cf (the Pi draws
// ASCII), have 1, whole-degree temp and code, and five "d weekday code hi lo" days. No place, or a failed forecast:
// "have 0". Breaking handleWeather (a wrong Open-Meteo field, a dropped route) fails this, not only the real Pis.
import { handleWeather } from "../../worker.js";

let failures = 0;
function check(label, cond, detail) {
  if (!cond) { console.log(`FAIL: ${label}${detail !== undefined ? "  " + JSON.stringify(detail) : ""}`); failures++; }
  else console.log(`ok: ${label}`);
}
const forecast = {
  current: { temperature_2m: -2.6, weather_code: 71 },
  daily: { time: ["2026-11-01", "2026-11-02", "2026-11-03", "2026-11-04", "2026-11-05"], weather_code: [71, 3, 0, 61, 2],
           temperature_2m_max: [1.4, 4.6, 7, 9.5, 6], temperature_2m_min: [-5.5, -1, 2, 3, 1] },
};
let asked = "";
globalThis.fetch = async (url) => { asked = String(url); return new Response(JSON.stringify(forecast), { status: 200 }); };
const req = cf => { const r = new Request("https://joshuatree.jaybulb.com/api/weather"); Object.defineProperty(r, "cf", { value: cf }); return r; };

const text = await (await handleWeather(req({ city: "Zürich", latitude: "47.37", longitude: "8.54" }))).text();
const lines = text.trim().split("\n");
check("asks Open-Meteo for the caller's own place", asked.startsWith("https://api.open-meteo.com/v1/forecast?latitude=47.37&longitude=8.54"), asked);
check("the city is ASCII, accents dropped", lines[0] === "city Zurich", lines[0]);
check("have 1, whole degrees (rounded), the WMO code", lines[1] === "have 1" && lines[2] === "temp -3" && lines[3] === "code 71", lines.slice(1, 4));
check("five days, weekday from the date (Nov 1 2026 is a Sunday, 0), rounded highs and lows",
      lines.slice(4).join("|") === "d 0 71 1 -5|d 1 3 5 -1|d 2 0 7 2|d 3 61 10 3|d 4 2 6 1", lines.slice(4));
check("every byte is printable ASCII or a newline (feeds.c keeps nothing else)", [...text].every(c => c === "\n" || (c >= " " && c <= "~")));

check("no place in request.cf: have 0", (await (await handleWeather(req({}))).text()) === "have 0\n");
globalThis.fetch = async () => new Response("down", { status: 503 });
check("a failed forecast: have 0", (await (await handleWeather(req({ city: "Vancouver", latitude: "49.2", longitude: "-123.1" }))).text()) === "have 0\n");

if (failures) { console.log(`FAIL: ${failures} /api/weather checks failed`); process.exit(1); }
console.log("PASS: /api/weather answers in WEATHER.TXT's lines for the caller's place, in ASCII, and have 0 when it cannot");
