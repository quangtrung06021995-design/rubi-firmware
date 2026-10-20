// Weather for the place the device connects from (Vercel geo headers) via Open-Meteo (no key).
const CODES = {
  0: 'trời quang', 1: 'gần như quang', 2: 'có mây rải rác', 3: 'nhiều mây', 45: 'có sương mù', 48: 'sương mù',
  51: 'mưa phùn nhẹ', 53: 'mưa phùn', 55: 'mưa phùn dày', 61: 'mưa nhẹ', 63: 'mưa vừa', 65: 'mưa to',
  71: 'tuyết nhẹ', 73: 'tuyết', 75: 'tuyết dày', 80: 'mưa rào nhẹ', 81: 'mưa rào', 82: 'mưa rào rất to',
  95: 'có dông', 96: 'dông kèm mưa đá', 99: 'dông mạnh kèm mưa đá',
};
module.exports = async (req, res) => {
  const h = req.headers;
  let lat = parseFloat(h['x-vercel-ip-latitude']);
  let lon = parseFloat(h['x-vercel-ip-longitude']);
  let city = h['x-vercel-ip-city'] ? decodeURIComponent(h['x-vercel-ip-city']) : '';
  if (isNaN(lat) || isNaN(lon)) { lat = 35.85; lon = 129.22; city = city || 'Gyeongju'; }
  res.setHeader('Content-Type', 'text/plain; charset=utf-8');
  try {
    const u = `https://api.open-meteo.com/v1/forecast?latitude=${lat}&longitude=${lon}` +
      '&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m' +
      '&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max&timezone=auto&forecast_days=1';
    const d = await (await fetch(u)).json();
    const c = d.current, dl = d.daily;
    const where = city ? `Ở ${city}` : 'Ở vị trí hiện tại';
    res.status(200).send(
      `${where} bây giờ ${CODES[c.weather_code] || 'không rõ'}, ${Math.round(c.temperature_2m)} độ C ` +
      `(cảm giác ${Math.round(c.apparent_temperature)} độ), độ ẩm ${c.relative_humidity_2m}%, ` +
      `gió ${Math.round(c.wind_speed_10m)} km/h. Hôm nay thấp nhất ${Math.round(dl.temperature_2m_min[0])} độ, ` +
      `cao nhất ${Math.round(dl.temperature_2m_max[0])} độ, khả năng mưa ${dl.precipitation_probability_max[0]}%.`);
  } catch (e) {
    res.status(200).send('Không lấy được thời tiết lúc này.');
  }
};
