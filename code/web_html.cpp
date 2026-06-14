#include "config_types.h"

const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SMS Forwarding</title>
  <style>
    :root {
      --ink: #171717;
      --body: #4d4d4d;
      --mute: #888888;
      --canvas: #ffffff;
      --canvas-soft: #fafafa;
      --canvas-soft-2: #f5f5f5;
      --hairline: #ebebeb;
      --hairline-strong: #a1a1a1;
      --link: #0070f3;
      --error: #ee0000;
      --warning-soft: #ffefcf;
      --sidebar-w: 220px;
      --radius-sm: 6px;
      --radius-md: 8px;
      --radius-pill: 100px;
      --shadow-card: 0 0 0 1px rgba(0,0,0,0.08), 0 1px 1px rgba(0,0,0,0.02), 0 2px 2px rgba(0,0,0,0.04);
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, sans-serif;
      font-size: 14px; font-weight: 400; line-height: 1.5;
      color: var(--ink); background: var(--canvas-soft);
      display: flex; min-height: 100vh;
    }

    /* Sidebar */
    .sidebar {
      position: fixed; top: 0; left: 0; bottom: 0; width: var(--sidebar-w);
      background: var(--ink); display: flex; flex-direction: column;
      z-index: 100; overflow-y: auto;
    }
    .sidebar-brand { padding: 22px 18px 16px; border-bottom: 1px solid rgba(255,255,255,0.08); }
    .sidebar-brand h2 { font-size: 17px; font-weight: 600; color: #fff; letter-spacing: -0.4px; }
    .sidebar-brand span { font-size: 10px; color: rgba(255,255,255,0.4); display: block; margin-top: 1px; font-family: 'SF Mono','Cascadia Code','JetBrains Mono','Consolas',monospace; }
    .sidebar-nav { flex: 1; padding: 10px; }
    .sidebar-nav a {
      display: flex; align-items: center; gap: 10px; padding: 9px 12px;
      border-radius: var(--radius-sm); color: rgba(255,255,255,0.6);
      font-size: 13px; font-weight: 500; text-decoration: none;
      transition: all 0.12s; margin-bottom: 1px; cursor: pointer;
    }
    .sidebar-nav a:hover { background: rgba(255,255,255,0.07); color: rgba(255,255,255,0.85); }
    .sidebar-nav a.active { background: rgba(255,255,255,0.12); color: #fff; }
    .sidebar-nav a .ico { font-size: 15px; width: 20px; text-align: center; flex-shrink: 0; }
    .sidebar-divider { height: 1px; background: rgba(255,255,255,0.08); margin: 8px 12px; }
    .sidebar-section-label { font-size: 10px; color: rgba(255,255,255,0.3); padding: 4px 16px 6px; text-transform: uppercase; letter-spacing: 0.6px; font-family: 'SF Mono','Cascadia Code','JetBrains Mono','Consolas',monospace; }
    .sidebar-footer { padding: 12px 16px; border-top: 1px solid rgba(255,255,255,0.08); }
    .sidebar-footer .btn { width: 100%; }

    /* Main */
    .main {
      margin-left: var(--sidebar-w); flex: 1; padding: 32px;
      max-width: 780px; width: 100%;
    }
    .page-title { font-size: 22px; font-weight: 600; color: var(--ink); letter-spacing: -0.5px; margin-bottom: 6px; }
    .page-subtitle { font-size: 13px; color: var(--mute); margin-bottom: 24px; }

    /* Card */
    .card { background: var(--canvas); border-radius: var(--radius-md); box-shadow: var(--shadow-card); margin-bottom: 18px; }
    .card-header { padding: 16px 22px 0; font-size: 14px; font-weight: 600; color: var(--ink); letter-spacing: -0.2px; display: flex; align-items: center; gap: 8px; }
    .card-body { padding: 16px 22px 22px; }
    .card-header + .card-body { padding-top: 12px; }

    /* Panel hide/show */
    .panel { display: none; }
    .panel.active { display: block; }

    /* Form */
    .form-group { margin-bottom: 14px; }
    .form-group:last-child { margin-bottom: 0; }
    .form-label { display: block; font-size: 12px; font-weight: 500; color: var(--body); margin-bottom: 4px; letter-spacing: -0.1px; }
    .form-input, .form-select, .form-textarea {
      width: 100%; padding: 7px 11px; font-size: 13px; font-family: inherit;
      border: 1px solid var(--hairline); border-radius: var(--radius-sm);
      background: var(--canvas); color: var(--ink);
      transition: border-color 0.15s, box-shadow 0.15s; outline: none;
    }
    .form-input:focus, .form-select:focus, .form-textarea:focus { border-color: var(--ink); box-shadow: 0 0 0 1px var(--ink); }
    .form-select { cursor: pointer; }
    .form-textarea { resize: vertical; min-height: 70px; line-height: 1.5; }
    .form-hint { font-size: 11px; color: var(--mute); margin-top: 3px; line-height: 1.4; }
    .form-warning { font-size: 11px; color: #ab570a; background: var(--warning-soft); padding: 9px 12px; border-radius: var(--radius-sm); margin-bottom: 14px; line-height: 1.5; }
    .form-row { display: flex; gap: 14px; }
    .form-row .form-group { flex: 1; }

    /* Buttons */
    .btn {
      display: inline-flex; align-items: center; justify-content: center; gap: 6px;
      padding: 7px 14px; font-size: 13px; font-weight: 500; font-family: inherit;
      border-radius: var(--radius-pill); border: none; cursor: pointer;
      transition: all 0.15s; line-height: 1.4; white-space: nowrap;
    }
    .btn:disabled { opacity: 0.5; cursor: not-allowed; }
    .btn-primary { background: var(--ink); color: #fff; }
    .btn-primary:hover { background: #2a2a2a; }
    .btn-secondary { background: var(--canvas); color: var(--ink); box-shadow: 0 0 0 1px var(--hairline); }
    .btn-secondary:hover { background: var(--canvas-soft-2); }
    .btn-danger { background: var(--error); color: #fff; }
    .btn-danger:hover { background: #c50000; }
    .btn-sm { padding: 4px 10px; font-size: 12px; border-radius: var(--radius-sm); }
    .btn-white { background: #fff; color: var(--ink); }
    .btn-white:hover { background: #f0f0f0; }
    .btn-block { width: 100%; justify-content: center; }
    .btn-save { padding: 10px 20px; font-size: 14px; margin-top: 4px; }

    /* Push Channel */
    .push-channel { border: 1px solid var(--hairline); border-radius: var(--radius-md); padding: 14px; margin-bottom: 10px; background: var(--canvas-soft); transition: border-color 0.15s; }
    .push-channel:hover { border-color: var(--hairline-strong); }
    .push-channel-header { display: flex; align-items: center; gap: 8px; margin-bottom: 10px; }
    .push-channel-header label { font-size: 13px; font-weight: 600; color: var(--ink); cursor: pointer; }
    .push-channel-header input[type="checkbox"] { width: 15px; height: 15px; accent-color: var(--ink); }
    .push-channel-body { display: none; }
    .push-channel.enabled .push-channel-body { display: block; }
    .push-channel.enabled { border-color: var(--hairline-strong); background: var(--canvas); }
    .push-channel-body .form-group { margin-bottom: 12px; }
    .push-channel-body .form-group:last-child { margin-bottom: 0; }
    .push-channel-body label { display: block; font-size: 12px; font-weight: 500; color: var(--body); margin-bottom: 4px; letter-spacing: -0.1px; }
    .push-channel-body input[type="text"], .push-channel-body input[type="password"], .push-channel-body select, .push-channel-body textarea {
      width: 100%; padding: 7px 11px; font-size: 13px; font-family: inherit;
      border: 1px solid var(--hairline); border-radius: var(--radius-sm);
      background: var(--canvas); color: var(--ink);
      transition: border-color 0.15s, box-shadow 0.15s; outline: none;
    }
    .push-channel-body input:focus, .push-channel-body select:focus, .push-channel-body textarea:focus { border-color: var(--ink); box-shadow: 0 0 0 1px var(--ink); }
    .push-channel-body select { cursor: pointer; }
    .push-channel-body textarea { resize: vertical; min-height: 60px; line-height: 1.5; }
    .push-type-hint { font-size: 11px; color: var(--body); margin-top: 4px; padding: 8px 12px; background: var(--canvas-soft-2); border-radius: var(--radius-sm); font-family: 'SF Mono','Cascadia Code','JetBrains Mono','Consolas',monospace; line-height: 1.5; }

    /* Result Boxes */
    .result-box { margin-top: 12px; padding: 10px 14px; border-radius: var(--radius-sm); display: none; font-size: 12px; line-height: 1.5; }
    .result-success { background: #e8f5e9; color: #2e7d32; display: block; }
    .result-error { background: #ffebee; color: #c62828; display: block; }
    .result-loading { background: #fff3e0; color: #e65100; display: block; }
    .result-info { background: #e3f2fd; color: #1565c0; display: block; }
    .info-table { width: 100%; border-collapse: collapse; margin-top: 4px; font-size: 12px; }
    .info-table td { padding: 5px 8px; border-bottom: 1px solid var(--hairline); }
    .info-table td:first-child { font-weight: 500; color: var(--body); width: 40%; }

    /* Overview */
    .overview-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 14px; }
    .overview-item { background: var(--canvas-soft); border-radius: var(--radius-sm); padding: 14px; }
    .overview-item .label { font-size: 10px; color: var(--mute); text-transform: uppercase; letter-spacing: 0.4px; font-family: 'SF Mono','Cascadia Code','JetBrains Mono','Consolas',monospace; margin-bottom: 4px; }
    .overview-item .value { font-size: 14px; font-weight: 600; color: var(--ink); }

    /* Tools */
    .btn-row { display: flex; gap: 8px; flex-wrap: wrap; }
    .btn-row .btn { flex: 1; min-width: 90px; }
    .btn-row + .btn-row { margin-top: 8px; }
    #atLog {
      background: var(--ink); color: #50e3c2; font-family: 'SF Mono','Cascadia Code','JetBrains Mono','Consolas',monospace;
      min-height: 130px; max-height: 260px; overflow-y: auto; padding: 12px 14px;
      border-radius: var(--radius-sm); margin-bottom: 10px; font-size: 12px;
      white-space: pre-wrap; word-break: break-all; line-height: 1.5;
    }
    .at-bar { display: flex; gap: 6px; }
    .at-bar input { flex: 1; font-family: 'SF Mono','Cascadia Code','JetBrains Mono','Consolas',monospace; }
    .at-bar .btn { min-width: 60px; }

    /* Responsive */
    @media (max-width: 700px) {
      .sidebar { width: 50px; }
      .sidebar-brand h2 { font-size: 0; }
      .sidebar-brand h2::first-letter { font-size: 16px; }
      .sidebar-brand span, .sidebar-section-label { display: none; }
      .sidebar-nav a { padding: 10px; justify-content: center; }
      .sidebar-nav a span:not(.ico) { display: none; }
      .sidebar-nav a .ico { font-size: 16px; }
      .sidebar-divider { margin: 6px 8px; }
      .sidebar-footer { padding: 8px; }
      .sidebar-footer .btn span { display: none; }
      .sidebar-footer .btn { padding: 6px; font-size: 11px; }
      .main { margin-left: 50px; padding: 18px 14px; }
      :root { --sidebar-w: 50px; }
      .overview-grid { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
  <aside class="sidebar">
    <div class="sidebar-brand">
      <h2>SMS FWD</h2>
      <span>短信转发器</span>
    </div>
    <nav class="sidebar-nav">
      <div class="sidebar-section-label">配置</div>
      <a data-panel="overview" class="active"><span class="ico">🏠</span> <span>系统概览</span></a>
      <a data-panel="account"><span class="ico">🔐</span> <span>账号管理</span></a>
      <a data-panel="email"><span class="ico">📧</span> <span>邮件通知</span></a>
      <a data-panel="push"><span class="ico">🔗</span> <span>推送通道</span></a>
      <a data-panel="admin"><span class="ico">👤</span> <span>管理员 &amp; 黑名单</span></a>
      <div class="sidebar-divider"></div>
      <div class="sidebar-section-label">工具</div>
      <a data-panel="sendsms"><span class="ico">📤</span> <span>发送短信</span></a>
      <a data-panel="diagnose"><span class="ico">📊</span> <span>模组诊断</span></a>
      <a data-panel="network"><span class="ico">🌐</span> <span>网络测试</span></a>
      <a data-panel="modem"><span class="ico">✈</span> <span>模组控制</span></a>
      <a data-panel="atterm"><span class="ico">💻</span> <span>AT 终端</span></a>
      <a data-panel="log"><span class="ico">📋</span> <span>系统日志</span></a>
    </nav>
    <div class="sidebar-footer">
      <button class="btn btn-white btn-sm btn-block" onclick="switchPanel('account')"><span>修改密码</span> 🔑</button>
    </div>
  </aside>

  <main class="main">

    <!-- ===== Overview ===== -->
    <div class="panel active" id="panel-overview">
      <h1 class="page-title">系统概览</h1>
      <p class="page-subtitle">设备状态与基本信息</p>
      <div class="card">
        <div class="card-header">📡 设备信息</div>
        <div class="card-body">
          <div class="overview-grid">
            <div class="overview-item"><div class="label">Device IP</div><div class="value" id="ovIp">%IP%</div></div>
            <div class="overview-item"><div class="label">WiFi SSID</div><div class="value" id="ovSsid">%WIFI_SSID%</div></div>
            <div class="overview-item"><div class="label">Free Heap</div><div class="value" id="ovHeap">%FREE_HEAP%</div></div>
            <div class="overview-item"><div class="label">Uptime</div><div class="value" id="ovUptime">%UPTIME%</div></div>
          </div>
        </div>
      </div>
      <div class="card">
        <div class="card-header">⚙ 配置状态</div>
        <div class="card-body">
          <table class="info-table">
            <tr><td>模组状态</td><td id="cfgModem">%MODEM_CHECK%</td></tr>
            <tr><td>邮件通知</td><td id="cfgEmail">%SMTP_CHECK%</td></tr>
            <tr><td>推送通道</td><td id="cfgPush">%PUSH_COUNT% 个已启用</td></tr>
            <tr><td>管理员号码</td><td>%ADMIN_PHONE%</td></tr>
          </table>
        </div>
      </div>
    </div>

    <!-- ===== Account ===== -->
    <div class="panel" id="panel-account">
      <h1 class="page-title">账号管理</h1>
      <p class="page-subtitle">修改 Web 管理界面的登录凭据</p>
      <form action="/save" method="POST" id="mainForm">
      <div class="card">
        <div class="card-header">🔐 登录凭据</div>
        <div class="card-body">
          <div class="form-warning">首次使用请立即修改默认密码！默认: )rawliteral" DEFAULT_WEB_USER " / " DEFAULT_WEB_PASS R"rawliteral(</div>
          <div class="form-row">
            <div class="form-group"><label class="form-label">管理账号</label><input class="form-input" type="text" name="webUser" value="%WEB_USER%" placeholder="admin"></div>
            <div class="form-group"><label class="form-label">管理密码</label><input class="form-input" type="password" name="webPass" value="%WEB_PASS%" placeholder="设置复杂密码"></div>
          </div>
        </div>
      </div>
      <button type="submit" class="btn btn-primary btn-block btn-save">保存配置</button>
      </form>
    </div>

    <!-- ===== Email ===== -->
    <div class="panel" id="panel-email">
      <h1 class="page-title">邮件通知</h1>
      <p class="page-subtitle">配置 SMTP 服务器以接收短信邮件通知</p>
      <form action="/save" method="POST" id="mainForm2">
      <div class="card">
        <div class="card-header">📧 SMTP 设置</div>
        <div class="card-body">
          <div class="form-row">
            <div class="form-group"><label class="form-label">SMTP 服务器</label><input class="form-input" type="text" name="smtpServer" value="%SMTP_SERVER%" placeholder="smtp.qq.com"></div>
            <div class="form-group"><label class="form-label">SMTP 端口</label><input class="form-input" type="number" name="smtpPort" value="%SMTP_PORT%" placeholder="465"></div>
          </div>
          <div class="form-row">
            <div class="form-group"><label class="form-label">邮箱账号</label><input class="form-input" type="text" name="smtpUser" value="%SMTP_USER%" placeholder="your@qq.com"></div>
            <div class="form-group"><label class="form-label">密码 / 授权码</label><input class="form-input" type="password" name="smtpPass" value="%SMTP_PASS%" placeholder="授权码"></div>
          </div>
          <div class="form-group"><label class="form-label">接收邮件地址</label><input class="form-input" type="text" name="smtpSendTo" value="%SMTP_SEND_TO%" placeholder="receiver@example.com"></div>
        </div>
      </div>
      <button type="submit" class="btn btn-primary btn-block btn-save">保存配置</button>
      </form>
    </div>

    <!-- ===== Push Channels ===== -->
    <div class="panel" id="panel-push">
      <h1 class="page-title">推送通道</h1>
      <p class="page-subtitle">最多 5 个独立推送通道，支持 POST JSON、Bark、钉钉、飞书、PushPlus、Server酱、Gotify、Telegram</p>
      <form action="/save" method="POST" id="mainForm3">
      <div class="card">
        <div class="card-header">🔗 通道配置</div>
        <div class="card-body">
          %PUSH_CHANNELS%
        </div>
      </div>
      <button type="submit" class="btn btn-primary btn-block btn-save">保存配置</button>
      </form>
    </div>

    <!-- ===== Admin & Blacklist ===== -->
    <div class="panel" id="panel-admin">
      <h1 class="page-title">管理员 &amp; 黑名单</h1>
      <p class="page-subtitle">远程控制权限与短信过滤</p>
      <form action="/save" method="POST" id="mainForm4">
      <div class="card">
        <div class="card-header">👤 管理员手机号</div>
        <div class="card-body">
          <div class="form-group">
            <input class="form-input" type="text" name="adminPhone" value="%ADMIN_PHONE%" placeholder="13800138000">
            <p class="form-hint">此号码可通过短信发送远程指令（SMS:号码:内容 发短信、RESET 重启）</p>
          </div>
        </div>
      </div>
      <div class="card">
        <div class="card-header">🚫 号码黑名单</div>
        <div class="card-body">
          <div class="form-group">
            <textarea class="form-textarea" name="numberBlackList" rows="5" placeholder="每行一个号码">%NUMBER_BLACK_LIST%</textarea>
            <p class="form-hint">黑名单号码发来的短信将被自动忽略</p>
          </div>
        </div>
      </div>
      <button type="submit" class="btn btn-primary btn-block btn-save">保存配置</button>
      </form>
    </div>

    <!-- ===== Send SMS ===== -->
    <div class="panel" id="panel-sendsms">
      <h1 class="page-title">发送短信</h1>
      <p class="page-subtitle">通过模组直接发送短信</p>
      <div class="card">
        <div class="card-header">📤 新建短信</div>
        <div class="card-body">
          <form action="/sendsms" method="POST" target="_self">
            <div class="form-group"><label class="form-label">目标号码</label><input class="form-input" type="text" name="phone" placeholder="13800138000" required></div>
            <div class="form-group"><label class="form-label">短信内容</label><textarea class="form-textarea" name="content" placeholder="输入短信内容..." required oninput="updateCount(this)"></textarea><p class="form-hint">已输入 <span id="charCount">0</span> 字符</p></div>
            <button type="submit" class="btn btn-primary" style="padding:9px 18px;">发送短信</button>
          </form>
        </div>
      </div>
    </div>

    <!-- ===== Diagnostics ===== -->
    <div class="panel" id="panel-diagnose">
      <h1 class="page-title">模组诊断</h1>
      <p class="page-subtitle">查询模组状态、SIM 卡与网络信息</p>
      <div class="card">
        <div class="card-header">📊 查询</div>
        <div class="card-body">
          <div class="btn-row"><button class="btn btn-secondary" onclick="queryInfo('ati')">固件信息</button><button class="btn btn-secondary" onclick="queryInfo('signal')">信号质量</button></div>
          <div class="btn-row"><button class="btn btn-secondary" onclick="queryInfo('siminfo')">SIM 卡信息</button><button class="btn btn-secondary" onclick="queryInfo('network')">网络状态</button><button class="btn btn-secondary" onclick="queryInfo('wifi')">WiFi 状态</button></div>
          <div class="result-box" id="queryResult"></div>
        </div>
      </div>
    </div>

    <!-- ===== Network Test ===== -->
    <div class="panel" id="panel-network">
      <h1 class="page-title">网络测试</h1>
      <p class="page-subtitle">通过模组数据连接测试网络连通性</p>
      <div class="card">
        <div class="card-header">🌐 Ping</div>
        <div class="card-body">
          <button class="btn btn-secondary" id="pingBtn" onclick="confirmPing()">Ping 8.8.8.8</button>
          <p class="form-hint">通过模组执行 Ping，消耗极少流量</p>
          <div class="result-box" id="pingResult"></div>
        </div>
      </div>
      <div class="card">
        <div class="card-header">📡 WiFi 控制</div>
        <div class="card-body">
          <button class="btn btn-danger" onclick="wifiRestart()">重启 WiFi</button>
          <p class="form-hint">断开当前 WiFi 连接并重新连接</p>
          <div class="result-box" id="wifiResult"></div>
        </div>
      </div>
    </div>

    <!-- ===== Modem Control ===== -->
    <div class="panel" id="panel-modem">
      <h1 class="page-title">模组控制</h1>
      <p class="page-subtitle">模组重启、飞行模式、信号查询等操作</p>
      <div class="card">
        <div class="card-header">🔄 模组重启</div>
        <div class="card-body">
          <div class="btn-row"><button class="btn btn-danger" onclick="modemAction('restart')">软重启 (AT+CFUN)</button><button class="btn btn-danger" onclick="modemAction('hardreset')">硬重启 (EN引脚)</button></div>
          <p class="form-hint">软重启发送 AT+CFUN=1,1 指令（15s 超时）；硬重启通过 EN 引脚断电后重新上电</p>
          <div class="result-box" id="modemRstResult"></div>
        </div>
      </div>
      <div class="card">
        <div class="card-header">📶 信号查询</div>
        <div class="card-body">
          <div class="btn-row"><button class="btn btn-primary" onclick="modemAction('signal')">查询信号强度</button><button class="btn btn-primary" onclick="modemAction('operator')">查询运营商</button><button class="btn btn-primary" onclick="modemAction('imei')">查询 IMEI</button></div>
          <div class="result-box" id="modemQueryResult"></div>
        </div>
      </div>
      <div class="card">
        <div class="card-header">✈ 飞行模式</div>
        <div class="card-body">
          <div class="btn-row"><button class="btn btn-danger" id="flightBtn" onclick="toggleFlightMode()">切换飞行模式</button><button class="btn btn-secondary" onclick="queryFlightMode()">查询状态</button></div>
          <p class="form-hint">飞行模式开启后模组射频关闭，无法收发短信</p>
          <div class="result-box" id="flightResult"></div>
        </div>
      </div>
    </div>

    <!-- ===== AT Terminal ===== -->
    <div class="panel" id="panel-atterm">
      <h1 class="page-title">AT 指令终端</h1>
      <p class="page-subtitle">直接向模组发送 AT 指令并接收响应</p>
      <div class="card">
        <div class="card-header">💻 终端</div>
        <div class="card-body">
          <div id="atLog">就绪 — 输入 AT 指令开始调试</div>
          <div class="at-bar"><input class="form-input" type="text" id="atCmd" placeholder="AT+CSQ"><button class="btn btn-primary btn-sm" onclick="sendAT()" id="atBtn">发送</button></div>
          <div class="btn-row" style="margin-top:8px;"><button class="btn btn-secondary btn-sm" onclick="clearATLog()">清空日志</button></div>
          <p class="form-hint">直接向模组串口发送指令并接收响应，请谨慎操作</p>
        </div>
      </div>
    </div>

    <!-- ===== System Log ===== -->
    <div class="panel" id="panel-log">
      <h1 class="page-title">系统日志</h1>
      <p class="page-subtitle">实时查看设备串口日志输出 <span id="logStatus" style="color:#4CAF50;">● 自动刷新中</span></p>
      <div class="card">
        <div class="card-header">📋 日志输出</div>
        <div class="card-body">
          <div id="logView" style="background:#1e1e1e;color:#d4d4d4;padding:12px;border-radius:8px;font-family:'Cascadia Code','Fira Code',Consolas,monospace;font-size:12px;line-height:1.6;max-height:60vh;overflow-y:auto;white-space:pre-wrap;word-break:break-all;min-height:300px;">加载中...</div>
          <div class="btn-row" style="margin-top:8px;">
            <button class="btn btn-secondary btn-sm" onclick="clearLogUI()">清空显示</button>
            <button class="btn btn-secondary btn-sm" onclick="refreshLog()">手动刷新</button>
            <label style="margin-left:8px;font-size:13px;cursor:pointer;"><input type="checkbox" id="logAuto" checked onchange="toggleLogAuto()"> 自动刷新</label>
          </div>
          <p class="form-hint">显示设备运行时输出的日志信息，每2秒自动刷新。日志最多保留最近120条。</p>
        </div>
      </div>
    </div>

  </main>

  <script>
    // ---- Panel switching ----
    function switchPanel(name) {
      document.querySelectorAll('.panel').forEach(function(p) { p.classList.remove('active'); });
      document.getElementById('panel-' + name).classList.add('active');
      document.querySelectorAll('.sidebar-nav a').forEach(function(a) { a.classList.remove('active'); });
      document.querySelector('.sidebar-nav a[data-panel="' + name + '"]').classList.add('active');
    }
    document.querySelectorAll('.sidebar-nav a').forEach(function(a) {
      a.addEventListener('click', function() { switchPanel(this.dataset.panel); });
    });

    // ---- Push Channel JS ----
    function toggleChannel(idx) {
      var ch = document.getElementById('channel' + idx);
      var cb = document.getElementById('push' + idx + 'en');
      if (cb.checked) ch.classList.add('enabled'); else ch.classList.remove('enabled');
    }
    function updateTypeHint(idx) {
      var sel = document.getElementById('push' + idx + 'type');
      var hint = document.getElementById('hint' + idx);
      var extra = document.getElementById('extra' + idx);
      var custom = document.getElementById('custom' + idx);
      var type = parseInt(sel.value);
      extra.style.display = 'none'; custom.style.display = 'none';
      document.getElementById('key1label' + idx).innerText = '参数 1';
      document.getElementById('key2label' + idx).innerText = '参数 2';
      document.getElementById('key1' + idx).placeholder = '';
      document.getElementById('key2' + idx).placeholder = '';
      var kg = document.getElementById('key2group' + idx);
      if (kg) kg.style.display = 'none';
      if (type == 1) hint.innerHTML = 'POST JSON<br>{"sender":"+8613800138000","message":"...","timestamp":"2026-01-01 12:00:00"}';
      else if (type == 2) hint.innerHTML = 'Bark (iOS)<br>POST {"title":"发送者","body":"短信内容"}';
      else if (type == 3) hint.innerHTML = 'GET 请求<br>URL?sender=xxx&message=xxx&timestamp=xxx';
      else if (type == 4) { hint.innerHTML = '钉钉机器人<br>填写 Webhook 地址，加签需填 Secret'; extra.style.display='block'; document.getElementById('key1label'+idx).innerText='Secret（加签密钥，可选）'; document.getElementById('key1'+idx).placeholder='SEC...'; }
      else if (type == 5) { hint.innerHTML = 'PushPlus<br>填写 Token，URL 留空使用默认'; extra.style.display='block'; document.getElementById('key1label'+idx).innerText='Token'; document.getElementById('key1'+idx).placeholder='pushplus token'; if(kg)kg.style.display='block'; document.getElementById('key2label'+idx).innerText='发送渠道'; document.getElementById('key2'+idx).placeholder='wechat / extension / app'; }
      else if (type == 6) { hint.innerHTML = 'Server酱<br>填写 SendKey，URL 留空使用默认'; extra.style.display='block'; document.getElementById('key1label'+idx).innerText='SendKey'; document.getElementById('key1'+idx).placeholder='SCT...'; }
      else if (type == 7) { hint.innerHTML = '自定义模板<br>使用 {sender} {message} {timestamp} 占位符'; custom.style.display='block'; }
      else if (type == 8) { hint.innerHTML = '飞书机器人<br>填写 Webhook 地址，签名验证需填 Secret'; extra.style.display='block'; document.getElementById('key1label'+idx).innerText='Secret（签名密钥，可选）'; document.getElementById('key1'+idx).placeholder='飞书签名密钥'; }
      else if (type == 9) { hint.innerHTML = 'Gotify<br>填写服务器地址 + 应用 Token'; extra.style.display='block'; document.getElementById('key1label'+idx).innerText='Token（应用 Token）'; document.getElementById('key1'+idx).placeholder='A...'; }
      else if (type == 10) { hint.innerHTML = 'Telegram Bot<br>Chat ID（参数1）+ Bot Token（参数2）'; extra.style.display='block'; document.getElementById('key1label'+idx).innerText='Chat ID'; document.getElementById('key1'+idx).placeholder='123456789'; if(kg)kg.style.display='block'; document.getElementById('key2label'+idx).innerText='Bot Token'; document.getElementById('key2'+idx).placeholder='12345678:ABC...'; }
    }
    document.addEventListener('DOMContentLoaded', function() {
      for (var i = 0; i < 5; i++) { toggleChannel(i); updateTypeHint(i); }
    });

    // ---- Send SMS ----
    function updateCount(el) { document.getElementById('charCount').textContent = el.value.length; }

    // ---- Query ----
    function queryInfo(type) {
      var r = document.getElementById('queryResult');
      r.className = 'result-box result-loading'; r.textContent = '查询中...';
      fetch('/query?type=' + type).then(function(rr){return rr.json()}).then(function(d){
        if(d.success){r.className='result-box result-info';r.innerHTML=d.message;}
        else{r.className='result-box result-error';r.innerHTML='查询失败: '+d.message;}
      }).catch(function(e){r.className='result-box result-error';r.textContent='请求失败: '+e;});
    }

    // ---- Ping ----
    function confirmPing(){if(confirm('确定要执行 Ping 吗？将消耗少量流量。'))doPing();}
    function doPing(){
      var b=document.getElementById('pingBtn'),r=document.getElementById('pingResult');
      b.disabled=true;b.textContent='Pinging...';
      r.className='result-box result-loading';r.textContent='正在 Ping 8.8.8.8（最长 30 秒）...';
      fetch('/ping',{method:'POST'}).then(function(rr){return rr.json()}).then(function(d){
        b.disabled=false;b.textContent='Ping 8.8.8.8';
        if(d.success){r.className='result-box result-success';r.innerHTML='Ping 成功 — '+d.message;}
        else{r.className='result-box result-error';r.innerHTML='Ping 失败 — '+d.message;}
      }).catch(function(e){b.disabled=false;b.textContent='Ping 8.8.8.8';r.className='result-box result-error';r.textContent='请求失败: '+e;});
    }

    // ---- WiFi Control ----
    function wifiRestart(){
      if(!confirm('确定要重启WiFi吗？网页将暂时不可用。'))return;
      var r=document.getElementById('wifiResult');
      r.className='result-box result-loading';r.textContent='WiFi 重启中（约5秒）...';
      fetch('/wifi?action=restart').then(function(rr){return rr.json()}).then(function(d){
        r.className=d.success?'result-box result-success':'result-box result-error';
        r.textContent=d.message;
      }).catch(function(e){r.className='result-box result-error';r.textContent='请求失败: '+e;});
    }

    // ---- Flight Mode ----
    function queryFlightMode(){
      var r=document.getElementById('flightResult');
      r.className='result-box result-loading';r.textContent='查询中...';
      fetch('/flight?action=query').then(function(rr){return rr.json()}).then(function(d){
        if(d.success){r.className='result-box result-info';r.innerHTML=d.message;}
        else{r.className='result-box result-error';r.innerHTML='查询失败: '+d.message;}
      }).catch(function(e){r.className='result-box result-error';r.textContent='请求失败: '+e;});
    }
    function toggleFlightMode(){
      if(!confirm('确定要切换飞行模式吗？'))return;
      var b=document.getElementById('flightBtn'),r=document.getElementById('flightResult');
      b.disabled=true;r.className='result-box result-loading';r.textContent='切换中...';
      fetch('/flight?action=toggle').then(function(rr){return rr.json()}).then(function(d){
        b.disabled=false;
        if(d.success){r.className='result-box result-success';r.innerHTML=d.message;}
        else{r.className='result-box result-error';r.innerHTML='切换失败: '+d.message;}
      }).catch(function(e){b.disabled=false;r.className='result-box result-error';r.textContent='请求失败: '+e;});
    }

    // ---- Modem Control ----
    function modemAction(action){
      var names={'restart':'软重启','hardreset':'硬重启','signal':'信号查询','operator':'运营商查询','imei':'IMEI查询'};
      var name=names[action]||action;
      var resultEl=null;
      if(action==='restart'||action==='hardreset') resultEl=document.getElementById('modemRstResult');
      else resultEl=document.getElementById('modemQueryResult');
      if(action==='hardreset'){
        if(!confirm('硬重启将断电重启模组，确定继续？'))return;
        resultEl.className='result-box result-loading';resultEl.textContent='硬重启中（约10秒）...';
        fetch('/modem?action=hardreset').then(function(rr){return rr.json()}).then(function(d){
          resultEl.className='result-box result-success';resultEl.textContent=d.message+' — 稍后请手动查询信号确认恢复';
        }).catch(function(e){resultEl.className='result-box result-error';resultEl.textContent='请求失败: '+e;});
        return;
      }
      resultEl.className='result-box result-loading';resultEl.textContent=name+'中...';
      fetch('/modem?action='+action).then(function(rr){return rr.json()}).then(function(d){
        if(d.success){resultEl.className='result-box result-success';resultEl.innerHTML=name+'成功: '+d.message;}
        else{resultEl.className='result-box result-error';resultEl.innerHTML=name+'失败: '+d.message;}
      }).catch(function(e){resultEl.className='result-box result-error';resultEl.textContent='请求失败: '+e;});
    }

    // ---- AT Terminal ----
    function addLog(msg,type){
      type=type||'resp';var log=document.getElementById('atLog'),div=document.createElement('div'),b=document.createElement('b');
      if(type==='user'){b.style.color='#fff';b.textContent='> ';}
      else if(type==='error'){b.style.color='#f44336';b.textContent='! ';}
      else{b.style.color='#50e3c2';b.textContent='';}
      div.appendChild(b);div.appendChild(document.createTextNode(msg));
      log.appendChild(div);log.scrollTop=log.scrollHeight;
    }
    function sendAT(){
      var inp=document.getElementById('atCmd'),cmd=inp.value.trim();if(!cmd)return;
      var btn=document.getElementById('atBtn');btn.disabled=true;btn.textContent='...';
      addLog(cmd,'user');inp.value='';
      fetch('/at?cmd='+encodeURIComponent(cmd)).then(function(rr){return rr.json()}).then(function(d){
        addLog(d.message,d.success?'resp':'error');
      }).catch(function(e){addLog('网络错误: '+e,'error')}).finally(function(){btn.disabled=false;btn.textContent='发送';});
    }
    function clearATLog(){var l=document.getElementById('atLog');l.innerHTML='';addLog('日志已清空','resp');}
    document.getElementById('atCmd').addEventListener('keydown',function(e){if(e.key==='Enter')sendAT();});

    // ---- Log Viewer ----
    var logTimer = null;
    function startLogPoll() {
      if (logTimer) return;
      logTimer = setInterval(refreshLog, 2000);
    }
    function stopLogPoll() {
      if (logTimer) { clearInterval(logTimer); logTimer = null; }
    }
    function toggleLogAuto() {
      if (document.getElementById('logAuto').checked) startLogPoll();
      else stopLogPoll();
    }
    function clearLogUI() { document.getElementById('logView').textContent = ''; }
    function refreshLog() {
      var el = document.getElementById('logView');
      fetch('/log').then(function(r) { return r.json(); }).then(function(lines) {
        if (!Array.isArray(lines)) return;
        el.textContent = lines.join('\n');
        el.scrollTop = el.scrollHeight;
      }).catch(function() { if (el.textContent === '加载中...') el.textContent = '无法获取日志'; });
    }
    var _origSwitchPanel = switchPanel;
    switchPanel = function(name) {
      _origSwitchPanel(name);
      if (name === 'log') { refreshLog(); startLogPoll(); }
      else stopLogPoll();
    };
    document.addEventListener('DOMContentLoaded', function() { refreshLog(); startLogPoll(); });
  </script>
</body>
</html>
)rawliteral";
