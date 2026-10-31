// Shared helpers for the Rubi store ("Kho"): password check and path rules.
const crypto = require('crypto');

function okPassword(given) {
  const want = process.env.ADMIN_PASSWORD || '';
  if (!want || typeof given !== 'string') return false;
  const a = crypto.createHash('sha256').update(given).digest();
  const b = crypto.createHash('sha256').update(want).digest();
  return crypto.timingSafeEqual(a, b);
}

// Paths inside the store: music/<name>.wav, truyen/<story>/<file>, hoc/<topic>/<file>. ASCII only (SD card safe).
function safeRel(p) {
  if (typeof p !== 'string' || p.length < 3 || p.length > 180) return false;
  if (!/^(music|truyen|hoc)\//.test(p)) return false;
  return p.split('/').every((seg) => seg.length > 0 && seg.length <= 80 && !seg.startsWith('.') && /^[A-Za-z0-9 _().,'+&-]+$/.test(seg));
}

module.exports = { okPassword, safeRel, PREFIX: 'kho/', DEL_PATH: 'meta/del.json' };
