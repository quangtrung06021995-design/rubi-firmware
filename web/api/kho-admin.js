// Admin actions (password in the JSON body): list what is stored, delete a file or a whole folder, and keep the delete list.
const { list, del, put } = require('@vercel/blob');
const { okPassword, safeRel, PREFIX, DEL_PATH } = require('./_kho');

async function listAll(prefix) {
  const out = [];
  let cursor;
  do {
    const r = await list({ prefix, limit: 1000, cursor });
    out.push(...r.blobs);
    cursor = r.hasMore ? r.cursor : undefined;
  } while (cursor);
  return out;
}

async function readDel() {
  const meta = (await list({ prefix: DEL_PATH, limit: 1 })).blobs[0];
  if (!meta) return [];
  try {
    const j = await (await fetch(meta.url + '?t=' + Date.now())).json();
    return Array.isArray(j) ? j : [];
  } catch (e) { return []; }
}

module.exports = async (req, res) => {
  try {
    const b = typeof req.body === 'string' ? JSON.parse(req.body) : (req.body || {});
    if (!okPassword(b.password)) return res.status(403).json({ error: 'Sai mật khẩu' });
    if (b.action === 'list') {
      const blobs = await listAll(PREFIX);
      let used = 0;
      const items = blobs.map((x) => { used += x.size; return { p: x.pathname.slice(PREFIX.length), s: x.size }; });
      return res.status(200).json({ items, used, limit: 1024 * 1024 * 1024 });
    }
    if (b.action === 'delete') {
      const target = String(b.path || '');
      if (!safeRel(target.replace(/\/$/, ''))) return res.status(400).json({ error: 'Đường dẫn không hợp lệ' });
      const clean = target.replace(/\/$/, '');
      const blobs = await listAll(PREFIX + clean);
      const mine = blobs.filter((x) => x.pathname === PREFIX + clean || x.pathname.startsWith(PREFIX + clean + '/'));
      if (mine.length) await del(mine.map((x) => x.url));
      const d = await readDel();
      if (!d.includes(clean)) d.push(clean);
      await put(DEL_PATH, JSON.stringify(d.slice(-300)), { access: 'public', addRandomSuffix: false, allowOverwrite: true, contentType: 'application/json', cacheControlMaxAge: 0 });
      return res.status(200).json({ ok: true, removed: mine.length });
    }
    res.status(400).json({ error: 'action?' });
  } catch (e) {
    res.status(500).json({ error: String(e.message || e) });
  }
};
