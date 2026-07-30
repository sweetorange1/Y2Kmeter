// ==========================================================
// iisaacbeats.cn/api/server.js
//   本地 Node.js API 服务 —— 遥测接收 + 更新检查
//
// 部署方式（腾讯云轻量服务器）：
//   1. 服务器上安装 Node.js（≥18）
//   2. cd /www/wwwroot/iisaacbeats.cn/api
//   3. npm install          # 安装依赖（better-sqlite3）
//   4. node server.js       # 手动启动测试
//   5. 使用 PM2 守护进程：pm2 start server.js --name y2k-api
//
// Nginx 配置（在 server 块中添加）：
//   location /api/ {
//       proxy_pass http://127.0.0.1:3001;
//       proxy_set_header Host $host;
//       proxy_set_header X-Real-IP $remote_addr;
//   }
//
// 工作原理：
//   用户/Y2KMeter 客户端 → https://iisaacbeats.cn/api/xxx
//     → Nginx 匹配 /api/ → 转发到本机 127.0.0.1:3001
//     → 本进程处理 SQLite 读写 → 返回 JSON 响应
//
// 注意：本文件替代了原来的 telemetry.js（Cloudflare Worker 格式）。
//      两者功能完全相同，只是运行环境从 Cloudflare 换成了 Node.js。
// ==========================================================

const http = require('http');
const Database = require('better-sqlite3');
const path = require('path');

// ---------- 配置 ----------
const PORT = 3001;
const HOST = '127.0.0.1'; // 仅监听本地，由 Nginx 反向代理对外暴露
const DB_PATH = path.join(__dirname, 'data.db');

// ---------- 初始化数据库 ----------
const db = new Database(DB_PATH);

// 开启 WAL 模式（提高并发性能）
db.pragma('journal_mode = WAL');

// 建表（与 schema.sql 保持一致）
db.exec(`
  CREATE TABLE IF NOT EXISTS telemetry (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    client_id          TEXT    NOT NULL,
    version            TEXT    NOT NULL DEFAULT '',
    build_type         TEXT    NOT NULL DEFAULT '',
    platform           TEXT    NOT NULL DEFAULT '',
    os                 TEXT    NOT NULL DEFAULT '',
    cpu_vendor         TEXT    NOT NULL DEFAULT '',
    cpu_model          TEXT    NOT NULL DEFAULT '',
    cpu_cores          INTEGER NOT NULL DEFAULT 0,
    ram_mb             INTEGER NOT NULL DEFAULT 0,
    plugin_mode        TEXT    NOT NULL DEFAULT '',
    host_name          TEXT    NOT NULL DEFAULT '',
    display_count      INTEGER NOT NULL DEFAULT 0,
    primary_display_w  INTEGER NOT NULL DEFAULT 0,
    primary_display_h  INTEGER NOT NULL DEFAULT 0,
    source_tag         TEXT    NOT NULL DEFAULT '',
    system_locale      TEXT    NOT NULL DEFAULT '',
    timezone_offset_min INTEGER NOT NULL DEFAULT 0,
    created_at         TEXT    NOT NULL DEFAULT (datetime('now'))
  );

  CREATE INDEX IF NOT EXISTS idx_telemetry_date ON telemetry(created_at);
  CREATE INDEX IF NOT EXISTS idx_telemetry_client_date ON telemetry(client_id, created_at);

  CREATE TABLE IF NOT EXISTS releases (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    version      TEXT    NOT NULL,
    platform     TEXT    NOT NULL DEFAULT 'all',
    download_url TEXT    NOT NULL DEFAULT '',
    changelog    TEXT    NOT NULL DEFAULT '',
    force_update INTEGER NOT NULL DEFAULT 0,
    created_at   TEXT    NOT NULL DEFAULT (datetime('now'))
  );
`);

// 插入种子数据（首次发布版本）
const seedStmt = db.prepare(
  `INSERT OR IGNORE INTO releases (version, platform, download_url, changelog, force_update)
   VALUES (?, ?, ?, ?, ?)`
);
seedStmt.run(
  '2.3.2', 'all',
  'https://pan.baidu.com/s/57uOC6W6lH_RHkzJ6x0ds5Q',
  'Telemetry & auto-update initial release.',
  0
);

console.log(`[api] 数据库已就绪: ${DB_PATH}`);

// ---------- 预编译 SQL 语句 ----------
const stmtCheckDup = db.prepare(
  `SELECT id FROM telemetry
   WHERE client_id = ? AND date(created_at) = ? LIMIT 1`
);

const stmtInsertTelemetry = db.prepare(`
  INSERT INTO telemetry
  (client_id, version, build_type, platform, os,
   cpu_vendor, cpu_model, cpu_cores, ram_mb,
   plugin_mode, host_name, display_count,
   primary_display_w, primary_display_h,
   source_tag, system_locale, timezone_offset_min,
   created_at)
  VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
`);

const stmtCheckUpdate = db.prepare(`
  SELECT version, download_url, changelog, force_update
  FROM releases
  WHERE platform = ? OR platform = 'all'
  ORDER BY created_at DESC LIMIT 1
`);

// ---------- 工具函数 ----------

function json(res, statusCode, data) {
  const body = JSON.stringify(data);
  res.writeHead(statusCode, {
    'Content-Type': 'application/json; charset=utf-8',
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
    'Content-Length': Buffer.byteLength(body)
  });
  res.end(body);
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
      try {
        resolve(body ? JSON.parse(body) : {});
      } catch (e) {
        reject(new Error('Invalid JSON'));
      }
    });
    req.on('error', reject);
  });
}

function compareVersions(a, b) {
  const pa = a.split(/[.-]/).map(Number);
  const pb = b.split(/[.-]/).map(Number);
  const len = Math.max(pa.length, pb.length);
  for (let i = 0; i < len; i++) {
    const va = isNaN(pa[i]) ? -1 : (pa[i] || 0);
    const vb = isNaN(pb[i]) ? -1 : (pb[i] || 0);
    if (va > vb) return 1;
    if (va < vb) return -1;
  }
  const suffixA = a.indexOf('-') > 0 ? a.slice(a.indexOf('-')) : '';
  const suffixB = b.indexOf('-') > 0 ? b.slice(b.indexOf('-')) : '';
  if (suffixA && !suffixB) return -1;
  if (!suffixA && suffixB) return 1;
  return 0;
}

function todayStr() {
  return new Date().toISOString().slice(0, 10);
}

// ---------- 路由处理 ----------

async function handleTelemetryPing(req, res) {
  try {
    const data = await readBody(req);
    const today = todayStr();
    const existing = stmtCheckDup.get(data.client_id, today);

    if (!existing) {
      stmtInsertTelemetry.run(
        data.client_id || '',
        data.version || '',
        data.build_type || '',
        data.platform || '',
        data.os || '',
        data.cpu_vendor || '',
        data.cpu_model || '',
        data.cpu_cores || 0,
        data.ram_mb || 0,
        data.plugin_mode || '',
        data.host_name || '',
        data.display_count || 0,
        data.primary_display_w || 0,
        data.primary_display_h || 0,
        data.source_tag || '',
        data.system_locale || '',
        data.timezone_offset_min || 0,
        new Date().toISOString()
      );
      console.log(`[telemetry] 记录: client=${data.client_id}, version=${data.version}`);
    } else {
      console.log(`[telemetry] 去重: client=${data.client_id} (今日已记录)`);
    }

    json(res, 200, { status: 'ok' });
  } catch (e) {
    console.error('[telemetry] 错误:', e.message);
    json(res, 400, { status: 'error', message: e.message });
  }
}

function handleUpdateCheck(req, res) {
  try {
    const url = new URL(req.url, `http://${req.headers.host}`);
    const clientVersion = url.searchParams.get('version') || '0.0.0';
    const clientPlatform = url.searchParams.get('platform') || 'unknown';

    const row = stmtCheckUpdate.get(clientPlatform);

    if (!row) {
      json(res, 200, { has_update: false });
      return;
    }

    const hasUpdate = compareVersions(row.version, clientVersion) > 0;

    console.log(`[update] 检查: client=${clientVersion}, latest=${row.version}, hasUpdate=${hasUpdate}`);

    json(res, 200, {
      has_update: hasUpdate,
      latest_version: row.version,
      download_url: row.download_url || '',
      changelog: row.changelog || '',
      force_update: row.force_update === 1
    });
  } catch (e) {
    console.error('[update] 错误:', e.message);
    json(res, 200, { has_update: false });
  }
}

// ---------- HTTP 服务器 ----------

const server = http.createServer((req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  const pathname = url.pathname;

  console.log(`[${new Date().toISOString()}] ${req.method} ${pathname}`);

  if (req.method === 'OPTIONS') {
    res.writeHead(204, {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type'
    });
    res.end();
    return;
  }

  if (pathname === '/api/telemetry/ping' && req.method === 'POST') {
    handleTelemetryPing(req, res);
    return;
  }

  if (pathname === '/api/update/check' && req.method === 'GET') {
    handleUpdateCheck(req, res);
    return;
  }

  json(res, 404, { error: 'Not Found', path: pathname });
});

server.listen(PORT, HOST, () => {
  console.log(`[api] 服务已启动: http://${HOST}:${PORT}`);
  console.log('[api] 等待 Nginx 转发 /api/* 请求...');
});

process.on('SIGINT', () => {
  console.log('\n[api] 正在关闭...');
  db.close();
  server.close(() => {
    console.log('[api] 已关闭');
    process.exit(0);
  });
});
