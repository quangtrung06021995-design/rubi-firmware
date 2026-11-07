// Time + UTC offset for the DevKit/watch clock: the offset follows the place the request comes from
// (Vercel geo header), falls back to Korea time (UTC+9). TZ_OFFSET_MIN forces a fixed offset.
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

module.exports = (req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  res.status(200).json({ t: Date.now(), tz: tzOffsetMinutes(req.headers['x-vercel-ip-timezone']) });
};
