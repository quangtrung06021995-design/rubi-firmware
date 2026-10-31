// Gives the admin page a short-lived upload permission (files go straight from the browser to the store).
const { handleUpload } = require('@vercel/blob/client');
const { okPassword, safeRel, PREFIX } = require('./_kho');

module.exports = async (req, res) => {
  try {
    const body = typeof req.body === 'string' ? JSON.parse(req.body) : req.body;
    const json = await handleUpload({
      body,
      request: req,
      onBeforeGenerateToken: async (pathname, clientPayload) => {
        if (!okPassword(clientPayload)) throw new Error('Sai mật khẩu');
        if (!pathname.startsWith(PREFIX) && pathname !== 'meta/del.json') throw new Error('Sai đường dẫn');
        if (pathname.startsWith(PREFIX) && !safeRel(pathname.slice(PREFIX.length))) throw new Error('Tên không hợp lệ');
        return {
          addRandomSuffix: false,
          allowOverwrite: true,
          maximumSizeInBytes: 80 * 1024 * 1024,
          cacheControlMaxAge: 60,
        };
      },
      onUploadCompleted: async () => {},
    });
    res.status(200).json(json);
  } catch (e) {
    res.status(400).json({ error: String(e.message || e) });
  }
};
