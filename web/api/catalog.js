// Public catalog read by Rubi: every file in the store with its download address and exact size, plus the delete list.
const { list } = require('@vercel/blob');
const { PREFIX, DEL_PATH, safeRel } = require('./_kho');

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

module.exports = async (req, res) => {
  try {
    const blobs = await listAll(PREFIX);
    const items = [];
    const have = new Set();
    for (const b of blobs) {
      const p = b.pathname.slice(PREFIX.length);
      if (!safeRel(p)) continue;
      items.push({ p, u: b.url, s: b.size });
      have.add(p);
    }
    let del = [];
    const meta = (await list({ prefix: DEL_PATH, limit: 1 })).blobs[0];
    if (meta) {
      try {
        const r = await fetch(meta.url + '?t=' + Date.now());
        const j = await r.json();
        if (Array.isArray(j)) del = j.filter((d) => safeRel(d) && !have.has(d)).slice(-300);
      } catch (e) { /* no delete list yet */ }
    }
    res.setHeader('Cache-Control', 's-maxage=120, stale-while-revalidate=600');
    res.setHeader('Content-Type', 'application/json; charset=utf-8');
    res.status(200).send(JSON.stringify({ v: 1, items, del }));
  } catch (e) {
    res.status(500).json({ error: 'catalog failed' });
  }
};
