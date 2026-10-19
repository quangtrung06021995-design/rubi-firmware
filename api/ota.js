// OTA endpoint for Rubi (Vercel serverless function).
// 1) forwards the device's request to the official Xiaozhi server so voice chat keeps working,
// 2) replaces the "firmware" part with the newest Rubi build published on branch "ota".
const UPSTREAM = process.env.UPSTREAM_OTA || 'https://api.tenclass.net/xiaozhi/ota/';
const MANIFEST = process.env.MANIFEST_URL ||
  'https://raw.githubusercontent.com/quangtrung06021995-design/rubi-firmware/ota/manifest.json';
const FORWARD = ['activation-version', 'device-id', 'client-id', 'serial-number', 'user-agent', 'accept-language'];

// UTC offset in minutes for an IANA zone name (e.g. "Asia/Ho_Chi_Minh" -> 420).
function tzOffsetMinutes(zone) {
  if (process.env.TZ_OFFSET_MIN) return Number(process.env.TZ_OFFSET_MIN);
  try {
    if (zone) {
      const p = new Intl.DateTimeFormat('en-US', { timeZone: zone, timeZoneName: 'longOffset' })
        .formatToParts(new Date()).find((x) => x.type === 'timeZoneName').value;
      const m = /GMT([+-])(\d{2}):?(\d{2})?/.exec(p);
      if (m) return (m[1] === '-' ? -1 : 1) * (Number(m[2]) * 60 + Number(m[3] || 0));
      if (p === 'GMT') return 0;
    }
  } catch (e) { /* fall through */ }
  return 540;
}

module.exports = async (req, res) => {
  let data = {};
  if (req.method === 'POST') {
    try {
      const headers = { 'Content-Type': 'application/json' };
      for (const h of FORWARD) if (req.headers[h]) headers[h] = req.headers[h];
      const body = typeof req.body === 'string' ? req.body : JSON.stringify(req.body || {});
      const r = await fetch(UPSTREAM, { method: 'POST', headers, body });
      data = await r.json();
    } catch (e) {
      data = {};
    }
  }
  try {
    const m = await (await fetch(MANIFEST, { cache: 'no-store' })).json();
    if (m && m.version && m.url) data.firmware = { version: m.version, url: m.url };
  } catch (e) { /* no manifest yet: leave firmware as the upstream answered */ }
  // Clock on the screen: follows the place the device connects from (Vercel geo header),
  // falls back to Korea time (UTC+9). TZ_OFFSET_MIN, if set, forces a fixed offset.
  const tz = tzOffsetMinutes(req.headers['x-vercel-ip-timezone']);
  data.server_time = { timestamp: Date.now(), timezone_offset: tz };
  res.setHeader('Content-Type', 'application/json');
  res.status(200).send(JSON.stringify(data));
};
