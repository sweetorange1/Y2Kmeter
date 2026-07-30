const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = 3456;
const PUBLIC_DIR = path.join(__dirname, 'public');
const PRESETS_DIR = path.join(__dirname, '..', '..', 'assets', 'milkdrop_presets');

const MIME = {
  '.html': 'text/html', '.js': 'text/javascript',
  '.css': 'text/css', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg',
  '.svg': 'image/svg+xml', '.milk': 'text/plain; charset=utf-8',
};

function serveStatic(filePath, res) {
  if (!fs.existsSync(filePath)) { res.writeHead(404); res.end('Not Found'); return; }
  const ext = path.extname(filePath);
  res.writeHead(200, { 'Content-Type': MIME[ext] || 'application/octet-stream' });
  fs.createReadStream(filePath).pipe(res);
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, `http://localhost:${PORT}`);
  const pathname = url.pathname;

  // API: list presets
  if (pathname === '/api/presets') {
    const files = fs.readdirSync(PRESETS_DIR)
      .filter(f => f.endsWith('.milk'))
      .sort((a, b) => a.localeCompare(b));
    res.writeHead(200, { 'Content-Type': 'application/json' });
    return res.end(JSON.stringify(files));
  }

  // API: raw .milk file
  if (pathname.startsWith('/api/preset/')) {
    const name = decodeURIComponent(pathname.slice('/api/preset/'.length));
    return serveStatic(path.join(PRESETS_DIR, name), res);
  }

  // Serve butterchurn from node_modules
  if (pathname === '/butterchurn.min.js') {
    return serveStatic(path.join(__dirname, 'node_modules', 'butterchurn', 'lib', 'butterchurn.min.js'), res);
  }

  // Serve from public/ (default to index.html)
  let filePath = path.join(PUBLIC_DIR, pathname === '/' ? 'index.html' : pathname);
  serveStatic(filePath, res);
});

server.listen(PORT, () => {
  const count = fs.readdirSync(PRESETS_DIR).filter(f => f.endsWith('.milk')).length;
  console.log(`Milkdrop Tester → http://localhost:${PORT}  (${count} presets)`);
});