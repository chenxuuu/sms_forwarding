#include "config_types.h"

const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN" data-theme="light">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SMS Forwarding</title>
  <style>
    /* ===== 方向 A · 场测仪 / Field-Test Instrument(暗色一等公民) =====
       纯 vanilla，无网页字体。个性来自等宽数据 + 琥珀单强调 + 1px 网格 + 矩形。
       选择器与旧版完全一致，仅换观感，后端契约(元素 ID / JS / 占位符)不变。 */
    :root {
      --mono: 'SF Mono','Cascadia Code','JetBrains Mono','Consolas','Liberation Mono',monospace;
      --bg: #0E1014; --grid: rgba(255,255,255,0.025);
      --canvas: #15181E; --canvas-soft: #1B1F27; --canvas-soft-2: #1B1F27; --inset: #0A0C0F;
      --hairline: #242A33; --hairline-strong: #333B47;
      --ink: #E7EAEF; --body: #AEB6C1; --mute: #8B93A0; --faint: #576070;
      --amber: #E8B23A; --amber-dim: #9C7A2C; --amber-glow: rgba(232,178,58,0.13);
      --link: #E8B23A; --error: #DC6E4E;
      --warning-soft: rgba(232,178,58,0.12);
      --sidebar-w: 234px;
      --radius-sm: 2px; --radius-md: 2px; --radius-pill: 2px;
      --shadow-card: none;
    }
    /* 浅色"纸面仪器"主题(独立调色，非反相) */
    html[data-theme="light"] {
      --grid: rgba(20,28,40,0.03);
      --bg: #E7EAEF;
      --canvas: #FFFFFF; --canvas-soft: #F2F4F7; --canvas-soft-2: #F2F4F7; --inset: #F6F8FA;
      --hairline: #D8DDE4; --hairline-strong: #C0C7D1;
      --ink: #191D24; --body: #3F4651; --mute: #5C6573; --faint: #9AA2AE;
      --amber: #B07208; --amber-dim: #7E520A; --amber-glow: rgba(176,114,8,0.10);
      --link: #B07208; --error: #B84E32;
      --warning-soft: rgba(176,114,8,0.09);
    }
    /* 信号量表(概览签名) */
    .sig-card .gauge { display: grid; grid-template-columns: 46px 1fr 96px; gap: 10px; align-items: center; margin-bottom: 9px; }
    .sig-card .gauge:last-child { margin-bottom: 0; }
    .sig-card .gl { font: 600 10px/1 var(--mono); letter-spacing: 0.06em; color: var(--mute); }
    .sig-card .track { position: relative; height: 9px; background: var(--inset); border: 1px solid var(--hairline); border-radius: 1px; overflow: hidden; }
    .sig-card .fill { position: absolute; left: 0; top: 0; bottom: 0; width: 0; background: var(--amber); transition: width 0.35s cubic-bezier(0.3,0.7,0.3,1); }
    .sig-card .fill.ok { background: #5CB585; } .sig-card .fill.warn { background: var(--amber); } .sig-card .fill.bad { background: #DC6E4E; }
    .sig-card .tk { position: absolute; top: 0; bottom: 0; width: 1px; background: var(--hairline-strong); opacity: 0.7; }
    .sig-card .gv { font: 600 12px/1 var(--mono); text-align: right; color: var(--ink); font-variant-numeric: tabular-nums; }
    /* 主题切换按钮 */
    .theme-toggle { display: flex; align-items: center; gap: 7px; width: 100%; padding: 7px 10px; margin-bottom: 10px;
      background: var(--canvas-soft); border: 1px solid var(--hairline-strong); border-radius: var(--radius-sm);
      color: var(--mute); font: 600 11px/1 inherit; cursor: pointer; }
    .theme-toggle:hover { color: var(--amber); border-color: var(--amber-dim); }
    .theme-toggle svg { width: 14px; height: 14px; stroke: currentColor; fill: none; stroke-width: 1.8; }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'PingFang SC', 'Microsoft YaHei', sans-serif;
      font-size: 13px; font-weight: 400; line-height: 1.5;
      color: var(--ink); background: var(--bg);
      background-image: linear-gradient(var(--grid) 1px, transparent 1px), linear-gradient(90deg, var(--grid) 1px, transparent 1px);
      background-size: 28px 28px;
      display: flex; min-height: 100vh; -webkit-font-smoothing: antialiased;
    }

    /* Sidebar */
    .sidebar {
      position: fixed; top: 0; left: 0; bottom: 0; width: var(--sidebar-w);
      background: var(--canvas); border-right: 1px solid var(--hairline);
      display: flex; flex-direction: column; z-index: 100; overflow-y: auto;
    }
    .sidebar-brand { padding: 16px 16px 14px; border-bottom: 1px solid var(--hairline); }
    .sidebar-brand h2 { font-size: 15px; font-weight: 600; color: var(--ink); letter-spacing: 0; }
    .sidebar-brand span { font-size: 10px; color: var(--faint); display: block; margin-top: 3px; font-family: var(--mono); }
    .sidebar-nav { flex: 1; padding: 8px; }
    .sidebar-nav a {
      display: flex; align-items: center; gap: 10px; padding: 7px 10px;
      border-radius: var(--radius-sm); color: var(--mute);
      font-size: 12.5px; font-weight: 500; text-decoration: none;
      transition: all 0.12s; margin-bottom: 1px; cursor: pointer; border-left: 2px solid transparent;
    }
    .sidebar-nav a:hover { background: var(--canvas-soft); color: var(--ink); }
    .sidebar-nav a.active { background: var(--canvas-soft); color: var(--ink); border-left-color: var(--amber); }
    .sidebar-nav a .ico { width: 20px; height: 18px; display: inline-flex; align-items: center; justify-content: center; flex-shrink: 0; }
    .sidebar-nav a .ico svg { width: 16px; height: 16px; stroke: currentColor; fill: none; stroke-width: 1.8; stroke-linecap: round; stroke-linejoin: round; }
    .sidebar-divider { height: 1px; background: var(--hairline); margin: 8px 12px; }
    .sidebar-section-label { font-size: 9.5px; color: var(--faint); padding: 12px 14px 6px; text-transform: uppercase; letter-spacing: 0.16em; font-family: var(--mono); }
    .sidebar-footer { padding: 12px 14px; border-top: 1px solid var(--hairline); }
    .sidebar-footer .btn { width: 100%; }
    .sidebar-about { font-size: 10.5px; color: var(--faint); line-height: 1.6; margin-bottom: 10px; }
    .sidebar-about b { color: var(--mute); font-weight: 600; font-size: 12px; display: block; margin-bottom: 2px; }
    .sidebar-about a { color: var(--mute); text-decoration: none; }
    .sidebar-about a:hover { color: var(--amber); }

    /* Main */
    .main { margin-left: var(--sidebar-w); flex: 1; padding: 24px 26px; max-width: 1480px; width: 100%; }
    .panel form { max-width: 780px; }
    .panel.col1 { max-width: 780px; }
    /* 流动(masonry)布局：两列各自独立排列，卡片间距统一 14px；左右不强制对齐高度起点，消除"为对齐右列而产生的硬间隔" */
    .card-grid { column-count: 2; column-gap: 14px; }
    .card-grid > * { break-inside: avoid; -webkit-column-break-inside: avoid; margin: 0 0 14px; }
    .page-title { font-size: 20px; font-weight: 600; color: var(--ink); letter-spacing: -0.01em; margin-bottom: 6px; }
    .page-subtitle { font-size: 12px; color: var(--mute); margin-bottom: 22px; }

    /* Card -> 模块 */
    .card { background: var(--canvas); border: 1px solid var(--hairline); border-radius: var(--radius-md); box-shadow: none; margin-bottom: 14px; }
    .card-header { padding: 11px 16px; font-size: 10px; font-weight: 600; color: var(--mute); letter-spacing: 0.12em; text-transform: uppercase; font-family: var(--mono); border-bottom: 1px solid var(--hairline); display: flex; align-items: center; gap: 8px; }
    .card-body { padding: 14px 16px; }
    .card-header + .card-body { padding-top: 14px; }

    .panel { display: none; }
    .panel.active { display: block; }

    /* Form */
    .form-group { margin-bottom: 13px; }
    .form-group:last-child { margin-bottom: 0; }
    .form-label { display: block; font-size: 11.5px; font-weight: 500; color: var(--body); margin-bottom: 5px; letter-spacing: 0; }
    .form-input, .form-select, .form-textarea {
      width: 100%; padding: 7px 10px; font-size: 13px; font-family: var(--mono);
      border: 1px solid var(--hairline); border-radius: var(--radius-sm);
      background: var(--inset); color: var(--ink);
      transition: border-color 0.15s; outline: none;
    }
    .form-input:focus, .form-select:focus, .form-textarea:focus { border-color: var(--amber-dim); box-shadow: none; }
    .form-select { cursor: pointer; }
    .form-textarea { resize: vertical; min-height: 70px; line-height: 1.5; font-size: 12px; }
    .form-hint { font-size: 11px; color: var(--faint); margin-top: 5px; line-height: 1.5; }
    .form-warning { font-size: 11.5px; color: var(--amber); background: var(--warning-soft); border: 1px solid var(--amber-dim); padding: 9px 12px; border-radius: var(--radius-sm); margin-bottom: 14px; line-height: 1.5; }
    .form-row { display: flex; gap: 14px; }
    .form-row .form-group { flex: 1; }

    /* Buttons — 矩形 */
    .btn {
      display: inline-flex; align-items: center; justify-content: center; gap: 6px;
      padding: 7px 13px; font-size: 12px; font-weight: 600; font-family: inherit;
      border-radius: var(--radius-sm); border: 1px solid var(--hairline-strong); cursor: pointer;
      background: var(--canvas-soft); color: var(--ink); transition: all 0.12s; line-height: 1.4; white-space: nowrap;
    }
    .btn:hover { border-color: var(--amber-dim); color: var(--amber); }
    .btn:disabled { opacity: 0.5; cursor: not-allowed; }
    .btn-primary { background: var(--amber); color: #1A1306; border-color: var(--amber); }
    .btn-primary:hover { background: #f2c155; color: #1A1306; }
    .btn-secondary { background: var(--canvas-soft); color: var(--ink); box-shadow: none; }
    .btn-secondary:hover { background: var(--canvas-soft); color: var(--amber); border-color: var(--amber-dim); }
    .btn-danger { background: var(--canvas-soft); color: var(--ink); border-color: var(--hairline-strong); }
    .btn-danger:hover { background: var(--canvas-soft); color: var(--error); border-color: var(--error); }
    .btn-sm { padding: 5px 9px; font-size: 11px; border-radius: var(--radius-sm); }
    .btn-white { background: var(--canvas-soft); color: var(--ink); }
    .btn-white:hover { background: var(--canvas-soft); color: var(--amber); border-color: var(--amber-dim); }
    .btn-block { width: 100%; justify-content: center; }
    .btn-save { padding: 9px 18px; font-size: 13px; margin-top: 4px; margin-bottom: 14px; }   /* 与卡片 14px 间距统一 */

    /* Push Channel */
    .push-channel { border: 1px solid var(--hairline); border-radius: var(--radius-sm); padding: 13px; margin-bottom: 10px; background: var(--canvas-soft); transition: border-color 0.15s; }
    .push-channel:hover { border-color: var(--hairline-strong); }
    .push-channel-header { display: flex; align-items: center; gap: 8px; margin-bottom: 10px; }
    .push-channel-header label { font-size: 12.5px; font-weight: 600; color: var(--ink); cursor: pointer; }
    .push-channel-header input[type="checkbox"] { width: 14px; height: 14px; accent-color: var(--amber); }
    .push-channel-body { display: none; }
    .push-channel.enabled .push-channel-body { display: block; }
    .push-channel.enabled { border-color: var(--amber-dim); background: var(--canvas); }
    .push-channel-body .form-group { margin-bottom: 12px; }
    .push-channel-body .form-group:last-child { margin-bottom: 0; }
    .push-channel-body label { display: block; font-size: 11.5px; font-weight: 500; color: var(--body); margin-bottom: 5px; }
    .push-channel-body input[type="text"], .push-channel-body input[type="password"], .push-channel-body select, .push-channel-body textarea {
      width: 100%; padding: 7px 10px; font-size: 13px; font-family: var(--mono);
      border: 1px solid var(--hairline); border-radius: var(--radius-sm);
      background: var(--inset); color: var(--ink); transition: border-color 0.15s; outline: none;
    }
    .push-channel-body input:focus, .push-channel-body select:focus, .push-channel-body textarea:focus { border-color: var(--amber-dim); box-shadow: none; }
    .push-channel-body select { cursor: pointer; }
    .push-channel-body textarea { resize: vertical; min-height: 60px; line-height: 1.5; font-size: 12px; }
    .push-type-hint { font-size: 11px; color: var(--body); margin-top: 5px; padding: 8px 11px; background: var(--inset); border-radius: var(--radius-sm); font-family: var(--mono); line-height: 1.5; }

    /* Result Boxes — 暗色调四态 */
    .result-box { margin-top: 11px; padding: 9px 12px; border-radius: var(--radius-sm); display: none; font-size: 11.5px; line-height: 1.5; border: 1px solid var(--hairline); }
    .result-success { background: rgba(92,181,133,0.11); color: #5CB585; border-color: transparent; display: block; }
    .result-error { background: rgba(220,110,78,0.12); color: #DC6E4E; border-color: transparent; display: block; }
    .result-loading { background: var(--amber-glow); color: var(--amber); border-color: transparent; display: block; }
    .result-info { background: var(--canvas-soft); color: var(--body); border-color: var(--hairline); display: block; }
    #panel-diagnose .result-box { display: block; height: 80px; overflow: auto; font-family: var(--mono); }
    #panel-diagnose .result-box:empty { background: var(--inset); border-style: dashed; }
    #panel-diagnose .result-box:empty::before { content: "等待操作…"; color: var(--faint); }
    /* 弹窗 */
    .modal-overlay { display: none; position: fixed; inset: 0; background: rgba(0,0,0,0.5); z-index: 100; align-items: center; justify-content: center; padding: 20px; }
    .modal-overlay.show { display: flex; }
    .modal { background: var(--canvas); border: 1px solid var(--hairline-strong); border-radius: var(--radius-md); box-shadow: 0 12px 40px rgba(0,0,0,0.5); width: 100%; max-width: 440px; padding: 20px 22px; }
    .modal-head { display: flex; align-items: center; justify-content: space-between; font-size: 15px; font-weight: 600; color: var(--ink); margin-bottom: 16px; }
    .modal-x { background: none; border: none; font-size: 22px; line-height: 1; color: var(--mute); cursor: pointer; padding: 0 4px; }
    .modal-foot { display: flex; justify-content: flex-end; gap: 8px; margin-top: 10px; }

    /* SMS messages + tabs */
    .msg-tabs { display: flex; gap: 2px; }
    .msg-tab { background: none; border: none; padding: 6px 12px; font-size: 12px; color: var(--mute); cursor: pointer; border-bottom: 2px solid transparent; font-family: inherit; font-weight: 600; }
    .msg-tab.active { color: var(--amber); border-bottom-color: var(--amber); }
    .msg { border: 1px solid var(--hairline); border-radius: var(--radius-sm); padding: 12px 14px; margin-bottom: 10px; }
    .msg:last-child { margin-bottom: 0; }
    .msg-head { display: flex; justify-content: space-between; align-items: center; gap: 8px; margin-bottom: 6px; }
    .msg-sender { font-weight: 600; font-size: 13px; color: var(--ink); font-family: var(--mono); }
    .msg-chip { font-size: 10px; padding: 3px 7px; border-radius: var(--radius-sm); white-space: nowrap; border: 1px solid var(--hairline-strong); color: var(--mute); background: var(--canvas-soft); font-family: var(--mono); letter-spacing: 0.04em; }
    .msg-chip.ok { color: #5CB585; background: rgba(92,181,133,0.11); border-color: transparent; }
    .msg-chip.bad { color: #DC6E4E; background: rgba(220,110,78,0.12); border-color: transparent; }
    .msg-chip.wait { color: var(--amber); background: var(--amber-glow); border-color: transparent; }
    .msg-chip.otp { color: var(--amber); background: var(--amber-glow); border-color: var(--amber-dim); cursor: pointer; }
    .msg-time { font-size: 10.5px; color: var(--faint); white-space: nowrap; margin-bottom: 6px; font-family: var(--mono); }
    .msg-body { font-size: 13px; color: var(--body); white-space: pre-wrap; word-break: break-all; line-height: 1.55; }
    .msg-body mark { background: var(--amber-glow); color: var(--amber); padding: 0 2px; border-radius: 1px; }
    .msg-empty { color: var(--faint); font-size: 12px; padding: 28px 0; text-align: center; font-family: var(--mono); }
    .otpcode { font: 700 24px/1 var(--mono); letter-spacing: 0.1em; color: var(--amber); padding: 8px 13px; background: var(--amber-glow); border: 1px dashed var(--amber-dim); border-radius: var(--radius-sm); cursor: pointer; white-space: nowrap; display: inline-block; }
    .otpcode:hover { background: rgba(232,178,58,0.2); }
    .drawer-bg { display: none; position: fixed; inset: 0; background: rgba(0,0,0,0.5); z-index: 99; }
    .drawer-bg.show { display: block; }
    .msg-drawer { position: fixed; top: 0; right: 0; bottom: 0; width: 440px; max-width: 92vw; background: var(--canvas); border-left: 1px solid var(--hairline-strong); z-index: 100; transform: translateX(100%); transition: transform 0.2s ease; display: flex; flex-direction: column; }
    .msg-drawer.show { transform: none; }
    .msg-drawer .dh { display: flex; align-items: center; gap: 10px; padding: 14px 16px; border-bottom: 1px solid var(--hairline); font: 600 10px/1 var(--mono); letter-spacing: 0.12em; text-transform: uppercase; color: var(--mute); }
    .msg-drawer .db { padding: 16px; overflow-y: auto; flex: 1; }
    .msg-drawer .x { margin-left: auto; background: none; border: 0; color: var(--mute); cursor: pointer; font-size: 20px; }
    .dk { font: 500 11px/1.5 var(--mono); color: var(--faint); }
    .dv { font-family: var(--mono); word-break: break-all; margin-bottom: 12px; }
    .dfull { background: var(--inset); border: 1px solid var(--hairline); border-radius: var(--radius-sm); padding: 12px; font-size: 13px; line-height: 1.7; white-space: pre-wrap; word-break: break-word; margin: 12px 0; }
    .dfull mark { background: var(--amber-glow); color: var(--amber); padding: 1px 3px; border-radius: 1px; }
    .info-table { width: 100%; border-collapse: collapse; margin-top: 2px; font-size: 12px; }
    .info-table td { padding: 7px 6px; border-bottom: 1px solid var(--hairline); }
    .info-table tr:last-child td { border-bottom: 0; }
    .info-table td:first-child { font-weight: 400; color: var(--mute); width: 42%; }
    .info-table td:last-child { font-family: var(--mono); font-variant-numeric: tabular-nums; }

    /* Overview KPI */
    .stat-row { display: grid; grid-template-columns: repeat(auto-fit, minmax(158px, 1fr)); gap: 12px; margin-bottom: 14px; }
    .stat { background: var(--canvas); border: 1px solid var(--hairline); border-radius: var(--radius-sm); box-shadow: none; padding: 13px 15px; }
    .stat .k { font-size: 9.5px; color: var(--faint); text-transform: uppercase; letter-spacing: 0.1em; font-family: var(--mono); margin-bottom: 8px; display: flex; align-items: center; gap: 6px; }
    .stat .v { font-size: 18px; font-weight: 700; color: var(--ink); letter-spacing: -0.01em; line-height: 1.15; font-family: var(--mono); font-variant-numeric: tabular-nums; }
    .stat .s { font-size: 10.5px; color: var(--faint); margin-top: 4px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; font-family: var(--mono); }
    .dot { width: 7px; height: 7px; border-radius: 50%; background: var(--mute); display: inline-block; flex: none; }
    .dot.ok { background: #5CB585; } .dot.warn { background: var(--amber); } .dot.bad { background: #DC6E4E; }
    .metric-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(112px, 1fr)); gap: 8px; }
    .metric { background: var(--inset); border: 1px solid var(--hairline); border-radius: var(--radius-sm); padding: 10px 12px; }
    .metric .k { font-size: 9px; color: var(--faint); text-transform: uppercase; letter-spacing: 0.1em; font-family: var(--mono); margin-bottom: 6px; }
    .metric .v { font-size: 15px; font-weight: 600; color: var(--ink); letter-spacing: 0; font-family: var(--mono); font-variant-numeric: tabular-nums; }

    /* Tools */
    .btn-row { display: flex; gap: 7px; flex-wrap: wrap; }
    .btn-row .btn { flex: 1; min-width: 90px; }
    .btn-row + .btn-row { margin-top: 7px; }
    #atLog {
      background: var(--inset); color: #5CB585; font-family: var(--mono); border: 1px solid var(--hairline);
      min-height: 130px; max-height: 260px; overflow-y: auto; padding: 12px 14px;
      border-radius: var(--radius-sm); margin-bottom: 10px; font-size: 12px;
      white-space: pre-wrap; word-break: break-all; line-height: 1.55;
    }
    .at-bar { display: flex; gap: 6px; }
    .at-bar input { flex: 1; font-family: var(--mono); }
    .at-bar .btn { min-width: 60px; }

    /* Responsive */
    @media (max-width: 700px) {
      .sidebar { width: 50px; }
      .sidebar-brand h2 { font-size: 0; }
      .sidebar-brand h2::first-letter { font-size: 16px; }
      .sidebar-brand span, .sidebar-section-label, .sidebar-about { display: none; }
      .sidebar-nav a { padding: 10px; justify-content: center; }
      .sidebar-nav a span:not(.ico) { display: none; }
      .sidebar-nav a .ico { font-size: 16px; }
      .sidebar-divider { margin: 6px 8px; }
      .sidebar-footer { padding: 8px; }
      .sidebar-footer .btn span { display: none; }
      .sidebar-footer .btn { padding: 6px; font-size: 11px; }
      .main { margin-left: 50px; padding: 18px 14px; }
      :root { --sidebar-w: 50px; }
      .card-grid { column-count: 1; }
    }
    /* 保存提示 toast（AJAX 原地保存，不跳页） */
    .save-toast{position:fixed;right:20px;top:20px;z-index:9999;padding:11px 18px;border-radius:8px;font-size:13px;font-weight:600;color:#fff;box-shadow:0 4px 16px rgba(0,0,0,.25);opacity:0;transform:translateY(-8px);transition:opacity .25s,transform .25s;pointer-events:none;}
    .save-toast.show{opacity:1;transform:translateY(0);}
    .save-toast.ok{background:#1a7f37;}
    .save-toast.err{background:#cf222e;}
  </style>
</head>
<body>
  <aside class="sidebar">
    <div class="sidebar-brand">
      <h2>短信转发器</h2>
      <span>ESP32-C3 控制台</span>
    </div>
    <nav class="sidebar-nav">
      <div class="sidebar-section-label">概览</div>
      <a data-panel="overview" class="active"><span class="ico"><svg viewBox="0 0 24 24"><path d="M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><path d="M9 22V12h6v10"/></svg></span> <span>系统概览</span></a>
      <a data-panel="sim"><span class="ico"><svg viewBox="0 0 24 24"><path d="M19 21H5a2 2 0 0 1-2-2V8l5-5h9a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2z"/><rect x="8" y="13" width="8" height="5" rx="1"/></svg></span> <span>网络</span></a>
      <div class="sidebar-divider"></div>
      <div class="sidebar-section-label">短信</div>
      <a data-panel="inbox"><span class="ico"><svg viewBox="0 0 24 24"><path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/></svg></span> <span>收发短信</span></a>
      <div class="sidebar-divider"></div>
      <div class="sidebar-section-label">转发设置</div>
      <a data-panel="push"><span class="ico"><svg viewBox="0 0 24 24"><path d="M10 13a5 5 0 0 0 7 0l3-3a5 5 0 0 0-7-7l-1 1"/><path d="M14 11a5 5 0 0 0-7 0l-3 3a5 5 0 0 0 7 7l1-1"/></svg></span> <span>转发</span></a>
      <div class="sidebar-divider"></div>
      <div class="sidebar-section-label">诊断</div>
      <a data-panel="diagnose"><span class="ico"><svg viewBox="0 0 24 24"><path d="M22 12h-4l-3 9L9 3l-3 9H2"/></svg></span> <span>诊断与控制</span></a>
      <a data-panel="atterm"><span class="ico"><svg viewBox="0 0 24 24"><path d="M4 17l6-6-6-6"/><path d="M12 19h8"/></svg></span> <span>AT 终端</span></a>
      <a data-panel="log"><span class="ico"><svg viewBox="0 0 24 24"><path d="M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01"/></svg></span> <span>系统日志</span></a>
      <div class="sidebar-divider"></div>
      <div class="sidebar-section-label">自动化</div>
      <a data-panel="keepalive"><span class="ico"><svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="18" rx="2"/><path d="M16 2v4M8 2v4M3 10h18"/><circle cx="12" cy="15" r="3"/></svg></span> <span>定时任务</span></a>
      <div class="sidebar-divider"></div>
      <div class="sidebar-section-label">系统</div>
      <a data-panel="settings"><span class="ico"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg></span> <span>系统</span></a>
    </nav>
    <!-- 主题切换已移除：固定浅色 -->
  </aside>

  <main class="main">

    <!-- ===== Overview ===== -->
    <div class="panel active" id="panel-overview">
      <h1 class="page-title">系统概览</h1>
      <p class="page-subtitle">设备信号、转发与系统状态遥测</p>
      <!-- 信号量表(签名) -->
      <div class="card sig-card" style="margin-bottom:14px">
          <div class="card-header"><span class="dot" id="dotSig"></span>信号 SIGNAL<span id="ovRefresh" style="margin-left:auto;color:var(--faint);font-size:10.5px;font-family:var(--mono);font-weight:400;">设备 --</span></div>
        <div class="card-body">
          <div class="gauge"><span class="gl">CSQ</span><span class="track"><i class="tk" style="left:26%"></i><i class="tk" style="left:45%"></i><span class="fill" id="gCsq"></span></span><span class="gv" id="gCsqV">--</span></div>
          <div class="gauge"><span class="gl">RSSI</span><span class="track"><i class="tk" style="left:25%"></i><i class="tk" style="left:58%"></i><span class="fill" id="gRssi"></span></span><span class="gv" id="gRssiV">--</span></div>
          <div class="gauge"><span class="gl">RSRP</span><span class="track"><i class="tk" style="left:20%"></i><i class="tk" style="left:50%"></i><span class="fill" id="gRsrp"></span></span><span class="gv" id="gRsrpV">--</span></div>
          <div class="gauge"><span class="gl">RSRQ</span><span class="track"><i class="tk" style="left:29%"></i><i class="tk" style="left:59%"></i><span class="fill" id="gRsrq"></span></span><span class="gv" id="gRsrqV">--</span></div>
          <div class="gauge"><span class="gl">SINR</span><span class="track"><i class="tk" style="left:33%"></i><i class="tk" style="left:60%"></i><span class="fill" id="gSinr"></span></span><span class="gv" id="gSinrV">--</span></div>
          <div class="gauge"><span class="gl">WiFi</span><span class="track"><i class="tk" style="left:30%"></i><i class="tk" style="left:50%"></i><span class="fill" id="gWifi"></span></span><span class="gv" id="gWifiV">--</span></div>
        </div>
      </div>
      <!-- KPI 指标条 -->
      <div class="stat-row">
        <div class="stat"><div class="k"><span class="dot" id="dotData"></span>蜂窝数据</div><div class="v" id="ovData">--</div><div class="s" id="ovDataSub">--</div></div>
        <div class="stat"><div class="k">累计处理</div><div class="v" id="ovSms">--</div><div class="s" id="ovLastSms">--</div></div>
        <div class="stat"><div class="k">收件箱</div><div class="v" id="ovInbox">--</div><div class="s">本地留存</div></div>
      </div>
      <!-- 最新接收 / 验证码 hero -->
      <div class="card" id="otpHeroCard" style="display:none;margin-bottom:14px;border-left:2px solid var(--amber);">
        <div class="card-header"><span class="dot ok"></span>最新接收　验证码<a class="btn btn-secondary btn-sm" style="margin-left:auto;" onclick="switchPanel('inbox')">打开收件箱</a></div>
        <div class="card-body" style="display:flex;align-items:center;gap:16px;">
          <div style="flex:1;min-width:0;">
            <div class="mono" id="ohFrom" style="font-weight:600;margin-bottom:4px;"></div>
            <div id="ohText" style="font-size:12px;color:var(--mute);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;"></div>
            <div class="mono" id="ohWhen" style="font-size:10.5px;color:var(--faint);margin-top:6px;"></div>
          </div>
          <div class="otpcode" id="ohCode"></div>
        </div>
      </div>
      <!-- 详情：两列流动布局。左列首位=转发与系统 -->
      <div class="card-grid">
      <div>
        <div class="card">
          <div class="card-header">转发与系统</div>
          <div class="card-body">
            <table class="info-table">
              <tr><td>待转发 / 推送队列 / 待发短信 / 邮件队列</td><td id="tQueue">--</td></tr>
              <tr><td>运行时长</td><td id="dvUptime">%UPTIME%</td></tr>
              <tr><td>复位原因</td><td id="tReset">--</td></tr>
              <tr><td>时间同步</td><td id="tTime">--</td></tr>
              <tr><td>邮件通知</td><td id="cfgEmail">%SMTP_CHECK%</td></tr>
              <tr><td>推送通道</td><td id="cfgPush">%PUSH_COUNT% 个已启用</td></tr>
              <tr><td>管理员号码</td><td>%ADMIN_PHONE%</td></tr>
            </table>
          </div>
        </div>
        <div class="card">
          <div class="card-header">SIM 卡信息</div>
          <div class="card-body">
            <table class="info-table">
              <tr><td>运营商</td><td id="tOp">--</td></tr>
              <tr><td>模组状态</td><td id="tModem">%MODEM_CHECK%</td></tr>
              <tr><td>上网 IP（蜂窝）</td><td id="tCellIp">--</td></tr>
              <tr><td>本机号码</td><td id="tPhone">--</td></tr>
              <tr><td>IMEI</td><td id="tImei">--</td></tr>
              <tr><td>ICCID</td><td id="tIccid">--</td></tr>
              <tr><td>IMSI</td><td id="tImsi">--</td></tr>
              <tr><td>APN</td><td id="tApn">--</td></tr>
            </table>
          </div>
        </div>
      </div>
      <div>
        <div class="card">
          <div class="card-header">设备 / 固件信息</div>
          <div class="card-body">
            <table class="info-table">
              <tr><td>制造商</td><td id="dvMfr">--</td></tr>
              <tr><td>模组型号</td><td id="dvModel">--</td></tr>
              <tr><td>模组固件</td><td id="dvFw">--</td></tr>
              <tr><td>主控固件</td><td id="dvEspVer">--</td></tr>
              <tr><td>空闲堆</td><td id="dvHeap">--</td></tr>
              <tr><td>最大可分配块</td><td id="tMaxBlock">--</td></tr>
              <tr><td>芯片温度（ESP32-C3 片内）</td><td id="dvTemp">--</td></tr>
            </table>
          </div>
        </div>
        <div class="card">
          <div class="card-header"><span class="dot" id="dotWifi"></span>WiFi 详细信息</div>
          <div class="card-body">
            <table class="info-table">
              <tr><td>当前 SSID</td><td id="wfSsid">--</td></tr>
              <tr><td>IP 地址</td><td id="wfIp">--</td></tr>
              <tr><td>网关</td><td id="wfGw">--</td></tr>
              <tr><td>子网掩码</td><td id="wfMask">--</td></tr>
              <tr><td>DNS 服务器</td><td id="wfDns">--</td></tr>
              <tr><td>MAC 地址</td><td id="wfMac">--</td></tr>
              <tr><td>路由器 BSSID</td><td id="wfBssid">--</td></tr>
              <tr><td>WiFi 信道</td><td id="wfChan">--</td></tr>
            </table>
          </div>
        </div>
      </div>
      </div>

    </div>

    <!-- ===== SIM / 网络 ===== -->
    <div class="panel" id="panel-sim">
      <h1 class="page-title">网络</h1>
      <p class="page-subtitle">WiFi 接入与蜂窝/SIM 设置。本机经 WiFi 转发，默认不开启蜂窝流量</p>
      <div class="card-grid">
        <form action="/save" method="POST" id="simFormEl">
        <input type="hidden" name="simForm" value="1">
        <div class="card">
          <div class="card-header">蜂窝数据设置</div>
          <div class="card-body">
            <div class="form-group"><label class="form-label"><input type="checkbox" name="dataEnabled" %DATA_CHECKED%> 允许蜂窝数据（流量）</label>
              <p class="form-hint" style="color:var(--error);font-weight:600">注意：开启会导致收不到短信，模组会掉为纯数据附着，丢失承载短信的 CS/SGs 信令域（漫游时尤其明显）。本机经 WiFi 转发，无需开流量，保持关闭即可。</p>
              <p class="form-hint">默认关闭=禁用 PDP，零流量。仅在需要保号 UDP 流量 / 蜂窝 IP 时临时开启。</p></div>
            <div class="form-group"><label class="form-label">APN</label><input class="form-input" type="text" name="apn" value="%APN%" placeholder="留空=运营商自动（如 cmnet）"></div>
            <div class="form-group"><label class="form-label">运营商</label>
              <input class="form-input" type="text" name="operatorPlmn" value="%OPERATOR_PLMN%" placeholder="留空 = 自动注册（推荐）" list="plmnList">
              <datalist id="plmnList">
                <option value="46000">中国移动</option>
                <option value="46001">中国联通</option>
                <option value="46011">中国电信</option>
                <option value="46015">中国广电</option>
                <option value="310260">T-Mobile US（国外示例）</option>
                <option value="23410">O2 UK（国外示例）</option>
              </datalist>
              <p class="form-hint">从列表选运营商或手动填 PLMN(MCC+MNC，国外同理)。仅能选 SIM 可接入的网络，锁定不可达会失网；不确定留空自动。</p></div>
          </div>
        </div>
        <button type="submit" class="btn btn-primary btn-block btn-save">保存 SIM 设置</button>
        </form>
        <div class="card">
          <div class="card-header">WiFi 网络<button class="btn btn-secondary btn-sm" style="margin-left:auto;" onclick="wifiScan()">扫描</button></div>
          <div class="card-body">
            <div class="form-group"><label class="form-label">选择网络</label><select class="form-select" id="wifiScanSel" onchange="wifiPick()"><option value="">点“扫描”获取周边 WiFi</option></select></div>
            <div class="form-group"><label class="form-label">WiFi 名称 (SSID)</label><input class="form-input" type="text" id="wifiSsidIn" placeholder="从上方选择或手动输入"></div>
            <div class="form-group"><label class="form-label">WiFi 密码</label><input class="form-input" type="password" id="wifiPassIn" placeholder="开放网络留空"></div>
            <div class="btn-row"><button class="btn btn-primary" onclick="wifiSave()">保存并重启接入</button><button class="btn btn-secondary" onclick="wifiRestart()">重连当前网络</button></div>
            <p class="form-hint">保存后设备重启接入新网络；若连接失败会自动重开配网热点供你重配</p>
            <div class="result-box" id="wifiCfgResult"></div>
            <div class="result-box" id="wifiResult"></div>
          </div>
        </div>
      </div>
    </div>

    <!-- ===== SMS (收 / 发) ===== -->
    <div class="panel" id="panel-inbox">
      <h1 class="page-title">收发短信</h1>
      <p class="page-subtitle">本地留存最近收发的短信（接收最多 %INBOX_MAX% 条，仅本地查看）。支持全文检索与验证码高亮</p>
      <div class="card">
        <div class="card-header">
          <div class="msg-tabs">
            <button type="button" class="msg-tab active" id="tabRecv" onclick="smsTab('recv')">已接收</button>
            <button type="button" class="msg-tab" id="tabSent" onclick="smsTab('sent')">已发送</button>
          </div>
          <input id="smsSearch" class="form-input" placeholder="搜索发件人或正文…" oninput="renderMessages()" style="max-width:240px;margin-left:auto;">
          <button class="btn btn-secondary btn-sm" onclick="loadMessages()">刷新</button>
          <button class="btn btn-primary btn-sm" onclick="openSmsModal()">发短信</button>
        </div>
        <div class="card-body">
          <div id="inboxList">加载中...</div>
        </div>
      </div>
    </div>

    <!-- ===== 系统（账号 / 时间 / 维护） ===== -->
    <div class="panel" id="panel-settings">
      <h1 class="page-title">系统</h1>
      <p class="page-subtitle">管理账号、设备时间与维护（重启 / 配置备份 / 固件升级）</p>
      <div class="card-grid">
        <form action="/save" method="POST" id="mainForm">
        <div class="card">
          <div class="card-header">管理账号</div>
          <div class="card-body">
            <div class="form-warning">首次使用请立即修改默认密码！默认: )rawliteral" DEFAULT_WEB_USER " / " DEFAULT_WEB_PASS R"rawliteral(</div>
            <div class="form-group"><label class="form-label">管理账号</label><input class="form-input" type="text" name="webUser" value="%WEB_USER%" placeholder="admin"></div>
            <div class="form-group"><label class="form-label">管理密码</label><input class="form-input" type="password" name="webPass" value="%WEB_PASS%" placeholder="设置复杂密码"></div>
            <button type="submit" class="btn btn-primary btn-block" style="margin-top:4px;">保存账号</button>
          </div>
        </div>
        </form>
        <form action="/save" method="POST">
        <input type="hidden" name="tzForm" value="1">
        <div class="card">
          <div class="card-header">设备时间</div>
          <div class="card-body">
            <div class="form-group"><label class="form-label">时区</label><select class="form-select" name="tzOffsetMin">%TZ_OPTIONS%</select></div>
            <div class="form-group"><label class="form-label">NTP 服务器</label><input class="form-input" type="text" name="ntpServer" value="%NTP%" placeholder="ntp.aliyun.com"></div>
            <p class="form-hint">用于设备对时与本地时间显示；保号倒计时依赖对时，修改 NTP 需重启生效</p>
            <button type="submit" class="btn btn-primary btn-block" style="margin-top:4px;">保存时间</button>
          </div>
        </div>
        </form>
        <div class="card">
          <div class="card-header">设备维护</div>
          <div class="card-body">
            <div class="btn-row"><button class="btn btn-secondary" onclick="sysReboot()">重启设备</button><button class="btn btn-danger" onclick="sysFactory()">恢复出厂</button></div>
            <p class="form-hint">恢复出厂将清空所有配置并重启（WiFi 凭据为固件内置，不受影响）</p>
            <div class="result-box" id="sysResult"></div>
          </div>
        </div>
        <div class="card">
          <div class="card-header">配置备份 / 恢复</div>
          <div class="card-body">
            <div class="btn-row"><button class="btn btn-secondary" onclick="doExport()">导出配置</button></div>
            <p class="form-hint">导出为文本文件（含凭据，请妥善保管）。恢复：粘贴备份内容后点导入</p>
            <textarea class="form-textarea" id="importBox" rows="4" placeholder="粘贴备份内容到此处..."></textarea>
            <div class="btn-row" style="margin-top:8px;"><button class="btn btn-primary btn-sm" onclick="doImport()">导入配置</button></div>
            <div class="result-box" id="importResult"></div>
          </div>
        </div>
        <div class="card">
          <div class="card-header">固件升级 (OTA)</div>
          <div class="card-body">
            <input class="form-input" type="file" id="otaFile" accept=".bin">
            <div class="btn-row" style="margin-top:8px;"><button class="btn btn-primary btn-sm" onclick="doOta()">上传并升级</button></div>
            <p class="form-hint">上传编译生成的 .bin 固件；升级中请勿断电，约需 30-60 秒</p>
            <div class="result-box" id="otaResult"></div>
          </div>
        </div>
      </div>
    </div>

    <!-- ===== 转发（通道 + 规则 + 过滤） ===== -->
    <div class="panel" id="panel-push">
      <h1 class="page-title">转发</h1>
      <p class="page-subtitle">邮件通知与最多 5 个推送通道（POST JSON、Bark、钉钉、飞书、PushPlus、Server酱、Gotify、Telegram）</p>
      <div class="card-grid">
      <div>
      <form action="/save" method="POST" id="mainForm2">
      <input type="hidden" name="emailForm" value="1">
      <div class="card">
        <div class="card-header">邮件通知 (SMTP)<label style="margin-left:auto;text-transform:none;font-family:var(--sans);font-size:11.5px;color:var(--fg);font-weight:500;cursor:pointer;"><input type="checkbox" name="emailEnabled" %EMAIL_EN%> 启用邮件转发</label></div>
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
      <button type="submit" class="btn btn-primary btn-block btn-save">保存邮件设置</button>
      </form>
      <form action="/save" method="POST" id="mainForm3">
      <input type="hidden" name="pushForm" value="1">
      <div class="card">
        <div class="card-header">推送通道<label style="margin-left:auto;text-transform:none;font-family:var(--sans);font-size:11.5px;color:var(--fg);font-weight:500;cursor:pointer;"><input type="checkbox" name="pushEnabled" %PUSH_EN%> 启用推送转发</label></div>
        <div class="card-body">
          %PUSH_CHANNELS%
          <button type="button" class="btn btn-secondary btn-block" id="addChannelBtn" onclick="addChannel()" style="margin-top:8px;">+ 添加推送通道</button>
        </div>
      </div>
      <button type="submit" class="btn btn-primary btn-block btn-save">保存推送通道</button>
      </form>
      </div>
      <div>
      <form action="/save" method="POST" id="mainForm4">
      <div class="card">
        <div class="card-header">管理员手机号</div>
        <div class="card-body">
          <div class="form-group">
            <input class="form-input" type="text" name="adminPhone" value="%ADMIN_PHONE%" placeholder="+447700900456">
            <p class="form-hint">此号码可通过短信发送远程指令（SMS:号码:内容 发短信、RESET 重启）</p>
          </div>
        </div>
      </div>
      <div class="card">
        <div class="card-header">号码黑名单</div>
        <div class="card-body">
          <div class="form-group">
            <textarea class="form-textarea" name="numberBlackList" rows="5" placeholder="每行一个号码">%NUMBER_BLACK_LIST%</textarea>
            <p class="form-hint">黑名单号码发来的短信将被自动忽略</p>
          </div>
        </div>
      </div>
      <button type="submit" class="btn btn-primary btn-block btn-save">保存权限与过滤</button>
      </form>
      <form action="/save" method="POST" onsubmit="serializeRules()">
        <textarea name="forwardRules" id="forwardRulesRaw" style="display:none;">%FORWARD_RULES%</textarea>
        <div class="card">
          <div class="card-header">转发规则<button type="button" class="btn btn-secondary btn-sm" style="margin-left:auto;" onclick="addRule()">+ 添加规则</button></div>
          <div class="card-body">
            <p class="form-hint" style="margin-bottom:10px;">按发件人 / 关键词 / 正则把短信分流到指定通道或丢弃，自上而下匹配、命中即止；无规则命中则转发到全部启用通道。</p>
            <div id="rulesList"></div>
          </div>
        </div>
        <button type="submit" class="btn btn-primary btn-block btn-save">保存转发规则</button>
      </form>
      </div>
      </div>
    </div>


    <!-- ===== 定时与时间 (保号 + 定时重启/心跳 + 时间设置) ===== -->
    <div class="panel" id="panel-keepalive">
      <h1 class="page-title">定时任务</h1>
      <p class="page-subtitle">SIM 保号、定时重启与每日心跳</p>
      <form action="/save" method="POST">
      <input type="hidden" name="kaForm" value="1">
      <input type="hidden" name="schedForm" value="1">
      <div class="card-grid">
        <div class="card">
          <div class="card-header">保号设置</div>
          <div class="card-body">
            <div class="form-group"><label class="form-label"><input type="checkbox" id="kaEnabled" name="kaEnabled"> 启用保号定时</label></div>
            <div class="form-group"><label class="form-label">触发周期（天）</label><input class="form-input" type="number" id="kaIntervalDays" name="kaIntervalDays" value="175"><p class="form-hint">建议小于运营商要求天数（如 giffgaff 180 天则设 175）</p></div>
            <div class="form-group"><label class="form-label">动作</label>
              <select class="form-select" id="kaAction" name="kaAction">
                <option value="1">蜂窝 UDP 流量（约48KB）</option>
                <option value="2">发送短信</option>
                <option value="3">USSD 查询</option>
              </select>
            </div>
            <div class="form-group"><label class="form-label">目标（短信号码 / USSD 码；流量保号时留空）</label><input class="form-input" type="text" id="kaTarget" name="kaTarget" placeholder="如 10086 或 *122#"></div>
            <p class="form-hint" id="kaCountdown">距下次保号: --</p>
          </div>
        </div>
        <div class="card">
          <div class="card-header">定时重启 / 每日心跳</div>
          <div class="card-body">
            <div class="form-group"><label class="form-label"><input type="checkbox" name="rebootEnabled" %RB_CHECKED%> 每日定时重启</label></div>
            <div class="form-group"><label class="form-label">重启时刻（本地小时 0-23）</label><input class="form-input" type="number" name="rebootHour" value="%RB_HOUR%" min="0" max="23"></div>
            <div class="form-group"><label class="form-label"><input type="checkbox" name="hbEnabled" %HB_CHECKED%> 每日心跳通知（邮件）</label></div>
            <div class="form-group"><label class="form-label">心跳时刻（本地小时 0-23）</label><input class="form-input" type="number" name="hbHour" value="%HB_HOUR%" min="0" max="23"></div>
            <p class="form-hint">定时重启在空闲时段执行；心跳用于确认设备存活</p>
          </div>
        </div>
        <div class="card">
          <div class="card-header">手动操作</div>
          <div class="card-body">
            <div class="btn-row"><button type="button" class="btn btn-secondary" onclick="kaRun()">立即执行一次</button><button type="button" class="btn btn-secondary" onclick="kaReset()">重置基准日为今天</button></div>
            <p class="form-hint">“立即执行”触发一次动作并把基准日更新为今天；“重置基准日”只更新计时</p>
            <div class="result-box" id="kaResult"></div>
          </div>
        </div>
      </div>
      <button type="submit" class="btn btn-primary btn-block btn-save">保存配置</button>
      </form>
    </div>

    <!-- ===== Diagnostics & Control ===== -->
    <div class="panel" id="panel-diagnose">
      <h1 class="page-title">诊断与控制</h1>
      <p class="page-subtitle">执行蜂窝流量、USSD、重启、飞行模式等操作</p>
      <div class="card-grid">
        <div class="card">
          <div class="card-header">蜂窝流量测试</div>
          <div class="card-body">
            <div class="at-bar"><input class="form-input" type="text" id="pingHost" value="223.5.5.5" placeholder="IP 或域名"><button class="btn btn-primary btn-sm" id="pingBtn" onclick="doPing()">发送约48KB</button></div>
            <p class="form-hint">临时启用蜂窝数据(PDP)，向目标 UDP 端口发送约 48KB 上行数据，完成后自动关闭</p>
            <div class="result-box" id="pingResult"></div>
          </div>
        </div>
        <div class="card">
          <div class="card-header">USSD 查询（如查余额）</div>
          <div class="card-body">
            <div class="at-bar"><input class="form-input" type="text" id="ussdCode" placeholder="如 *122# 或 *#1345#"><button class="btn btn-primary btn-sm" onclick="sendUssd()">查询</button></div>
            <p class="form-hint">通过 AT+CUSD 发起，可用于保号/查余额；结果依运营商而定</p>
            <div class="result-box" id="ussdResult"></div>
          </div>
        </div>
        <div class="card">
          <div class="card-header">模组重启</div>
          <div class="card-body">
            <div class="btn-row"><button class="btn btn-danger" onclick="modemAction('restart')">模组软重启</button><button class="btn btn-danger" onclick="modemAction('hardreset')">模组硬重启</button></div>
            <p class="form-hint"><b>软重启</b>：发送 AT+CFUN=1,1 让模组重新注册网络，约 15 秒、<b>不断电</b>，适合仍能响应 AT 但网络异常 / 长时间不收短信时。<br><b>硬重启</b>：通过 EN 引脚给模组断电再上电（约 10 秒），相当于拔插一次电，用于模组卡死、软重启无效。<br>重启的是<b>模组</b>，不是 ESP32 主控；主控重启在「系统维护」。</p>
            <div class="result-box" id="modemRstResult"></div>
          </div>
        </div>
        <div class="card">
          <div class="card-header">飞行模式</div>
          <div class="card-body">
            <div class="btn-row"><button class="btn btn-danger" id="flightBtn" onclick="toggleFlightMode()">切换飞行模式</button><button class="btn btn-secondary" onclick="queryFlightMode()">查询状态</button></div>
            <p class="form-hint">飞行模式开启后模组射频关闭，无法收发短信</p>
            <div class="result-box" id="flightResult"></div>
          </div>
        </div>
      </div>
    </div>

    <!-- ===== AT Terminal ===== -->
    <div class="panel" id="panel-atterm">
      <h1 class="page-title">AT 指令终端</h1>
      <p class="page-subtitle">直接向模组发送 AT 指令并接收响应</p>
      <div class="card">
        <div class="card-header">终端</div>
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
      <p class="page-subtitle">实时查看设备串口日志输出</p>
      <div class="card">
        <div class="card-header">日志输出</div>
        <div class="card-body">
          <div id="logView" style="background:#1e1e1e;color:#d4d4d4;padding:12px;border-radius:8px;font-family:'Cascadia Code','Fira Code',Consolas,monospace;font-size:12px;line-height:1.6;max-height:60vh;overflow-y:auto;white-space:pre-wrap;word-break:break-all;min-height:300px;">加载中...</div>
          <div class="btn-row" style="margin-top:8px;">
            <button class="btn btn-secondary btn-sm" onclick="clearLogUI()">清空显示</button>
            <button class="btn btn-secondary btn-sm" onclick="refreshLog()">手动刷新</button>
            <button class="btn btn-secondary btn-sm" onclick="location.href='/logdownload'">下载日志</button>
            <label style="margin-left:8px;font-size:13px;cursor:pointer;"><input type="checkbox" id="logAuto" checked onchange="toggleLogAuto()"> 自动刷新</label>
          </div>
          <p class="form-hint">显示设备运行时输出的日志信息，每2秒自动刷新。设备端保留最近 200 条，可“下载日志”导出全部。</p>
        </div>
      </div>
    </div>

    <!-- ===== 发短信 弹窗 ===== -->
    <div class="modal-overlay" id="smsModal" onclick="if(event.target===this)closeSmsModal()">
      <div class="modal">
        <div class="modal-head"><span>发送短信</span><button class="modal-x" onclick="closeSmsModal()" aria-label="关闭">&times;</button></div>
        <div class="form-group"><label class="form-label">目标号码</label><input class="form-input" type="text" id="smsPhone" placeholder="+447700900456"></div>
        <div class="form-group"><label class="form-label">短信内容</label><textarea class="form-textarea" id="smsContent" placeholder="输入短信内容..." oninput="updateCount(this)"></textarea><p class="form-hint">已输入 <span id="charCount">0</span> 字符</p></div>
        <div class="result-box" id="smsSendResult"></div>
        <div class="modal-foot"><button class="btn btn-secondary" onclick="closeSmsModal()">取消</button><button class="btn btn-primary" id="smsSendBtn" onclick="sendSmsModal()">发送</button></div>
      </div>
    </div>

  </main>

  <div class="drawer-bg" id="drawerBg" onclick="closeMsgDrawer()"></div>
  <aside class="msg-drawer" id="msgDrawer">
    <div class="dh">短信详情<button class="x" onclick="closeMsgDrawer()">&times;</button></div>
    <div class="db" id="drawerBody"></div>
  </aside>

  <script>
    // 主题固定为浅色(深色模式已移除)
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
      if (type == 1) hint.innerHTML = 'POST JSON<br>{"sender":"+447700900123","message":"...","timestamp":"2026-01-01 12:00:00"}';
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
    // ---- 推送通道：只显示已启用，其余由"添加推送通道"逐个展开 ----
    function setupChannels() {
      for (var i = 0; i < 5; i++) {
        var ch = document.getElementById('channel' + i), en = document.getElementById('push' + i + 'en');
        if (ch && en && !en.checked) ch.style.display = 'none';
      }
      updateAddBtn();
    }
    function updateAddBtn() {
      var btn = document.getElementById('addChannelBtn'); if (!btn) return;
      var hidden = 0;
      for (var i = 0; i < 5; i++) { var ch = document.getElementById('channel' + i); if (ch && ch.style.display === 'none') hidden++; }
      btn.style.display = hidden > 0 ? '' : 'none';
    }
    function addChannel() {
      for (var i = 0; i < 5; i++) {
        var ch = document.getElementById('channel' + i);
        if (ch && ch.style.display === 'none') {
          ch.style.display = '';
          var en = document.getElementById('push' + i + 'en');
          if (en) { en.checked = true; toggleChannel(i); }
          updateAddBtn();
          return;
        }
      }
    }
    document.addEventListener('DOMContentLoaded', function() {
      for (var i = 0; i < 5; i++) { toggleChannel(i); updateTypeHint(i); }
      setupChannels();
    });

    // ---- Push Channel Test ----
    var pushTestTimers = {};
    function pollTestPush(idx) {
      if (pushTestTimers[idx]) clearTimeout(pushTestTimers[idx]);
      fetch('/testpush?action=status&ch=' + idx).then(function(x){return x.json();}).then(function(d) {
        var r = document.getElementById('pushTestResult' + idx);
        if (d.queued || d.running) {
          r.className = 'result-box result-loading';
          r.textContent = d.message || '测试推送后台执行中...';
          pushTestTimers[idx] = setTimeout(function(){ pollTestPush(idx); }, 3000);
          return;
        }
        r.className = 'result-box ' + (d.success ? 'result-success' : 'result-error');
        r.textContent = d.message || (d.success ? '测试推送已发送' : '测试推送失败');
      }).catch(function(e){
        var r = document.getElementById('pushTestResult' + idx);
        r.className = 'result-box result-error';
        r.textContent = '状态查询失败: ' + e;
      });
    }
    function testPush(idx) {
      var r = document.getElementById('pushTestResult' + idx);
      r.className = 'result-box result-loading'; r.textContent = '测试推送已提交（请先保存配置）...';
      fetch('/testpush?ch=' + idx, { method: 'POST' }).then(function(x){return x.json();}).then(function(d) {
        if (d.success && d.queued) {
          r.textContent = d.message || '后台测试推送中...';
          pollTestPush(idx);
        } else {
          r.className = 'result-box result-error';
          r.textContent = d.message || '任务启动失败';
        }
      }).catch(function(e){ r.className = 'result-box result-error'; r.textContent = '请求失败: ' + e; });
    }

    // ---- USSD ----
    function sendUssd() {
      var code = document.getElementById('ussdCode').value.trim();
      if (!code) return;
      var r = document.getElementById('ussdResult');
      r.className = 'result-box result-loading'; r.textContent = '查询中（最长约 20 秒）...';
      fetch('/ussd?code=' + encodeURIComponent(code)).then(function(x){return x.json();}).then(function(d) {
        r.className = 'result-box ' + (d.success ? 'result-info' : 'result-error');
        r.textContent = d.message || '(无响应)';
      }).catch(function(e){ r.className = 'result-box result-error'; r.textContent = '请求失败: ' + e; });
    }

    // ---- Send SMS ----
    function updateCount(el) { document.getElementById('charCount').textContent = el.value.length; }
    // ---- 发短信弹窗 ----
    function openSmsModal() {
      var r = document.getElementById('smsSendResult'); r.className = 'result-box'; r.textContent = '';
      document.getElementById('smsModal').classList.add('show');
      document.getElementById('smsPhone').focus();
    }
    function closeSmsModal() { document.getElementById('smsModal').classList.remove('show'); }
    function sendSmsModal() {
      var phone = document.getElementById('smsPhone').value.trim();
      var content = document.getElementById('smsContent').value;
      var r = document.getElementById('smsSendResult');
      if (!phone) { r.className = 'result-box result-error'; r.textContent = '请输入目标号码'; return; }
      if (!content.trim()) { r.className = 'result-box result-error'; r.textContent = '请输入短信内容'; return; }
      var btn = document.getElementById('smsSendBtn'); btn.disabled = true;
      r.className = 'result-box result-loading'; r.textContent = '提交中...';
      var body = 'phone=' + encodeURIComponent(phone) + '&content=' + encodeURIComponent(content);
      fetch('/sendsms', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: body })
        .then(function(x){ return x.json(); }).then(function(d) {
          btn.disabled = false;
          r.className = 'result-box ' + (d.success ? 'result-success' : 'result-error'); r.textContent = d.message;
          if (d.success) {
            document.getElementById('smsContent').value = ''; updateCount(document.getElementById('smsContent'));
            loadStatus();
            if (smsBox === 'sent') setTimeout(loadMessages, 1200);
            setTimeout(closeSmsModal, 1000);
          }
        }).catch(function(e){ btn.disabled = false; r.className = 'result-box result-error'; r.textContent = '请求失败: ' + e; });
    }

    // ---- UDP cellular traffic ----
    var pingPollTimer = null;
    function doPing(){
      var b=document.getElementById('pingBtn'),r=document.getElementById('pingResult');
      var host=(document.getElementById('pingHost').value||'').trim()||'223.5.5.5';
      b.disabled=true;b.textContent='...';
      r.className='result-box result-loading';r.textContent='提交流量消耗任务...';
      fetch('/ping',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'host='+encodeURIComponent(host)}).then(function(rr){return rr.json()}).then(function(d){
        if(d.success && d.running){ r.textContent='后台发送约48KB中（会短暂开启蜂窝数据）...'; pollPingStatus(); }
        else{ b.disabled=false;b.textContent='发送约48KB'; r.className='result-box result-error'; r.textContent=d.message||'任务启动失败'; }
      }).catch(function(e){b.disabled=false;b.textContent='发送约48KB';r.className='result-box result-error';r.textContent='请求失败: '+e;});
    }
    function pollPingStatus(){
      if(pingPollTimer) clearTimeout(pingPollTimer);
      fetch('/ping?action=status',{method:'POST'}).then(function(rr){return rr.json()}).then(function(d){
        var b=document.getElementById('pingBtn'),r=document.getElementById('pingResult');
        if(d.running){
          r.className='result-box result-loading';
          r.textContent='正在向 '+(d.host||'')+' 发送约48KB UDP 流量（后台执行，可刷新网页）...';
          pingPollTimer=setTimeout(pollPingStatus,1000);
          return;
        }
        b.disabled=false;b.textContent='发送约48KB';
        if(d.success){r.className='result-box result-success';r.textContent='UDP 流量发送完成 — '+(d.message||'完成');}
        else{r.className='result-box result-error';r.textContent='UDP 流量发送失败 — '+(d.message||'无结果');}
      }).catch(function(e){
        var b=document.getElementById('pingBtn'),r=document.getElementById('pingResult');
        b.disabled=false;b.textContent='发送约48KB';r.className='result-box result-error';r.textContent='状态查询失败: '+e;
      });
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
      var names={'restart':'软重启','hardreset':'硬重启'};
      var name=names[action]||action;
      var resultEl=document.getElementById('modemRstResult');   // 仅 restart/hardreset 调用本函数
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
        if(d.success){
          resultEl.className='result-box result-success';resultEl.textContent=name+'成功: '+d.message;
        }
        else{resultEl.className='result-box result-error';resultEl.textContent=name+'失败: '+d.message;}
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

    // ---- Log Viewer (since 游标增量 + 后台标签页不轮询) ----
    var logTimer = null, logSince = 0, logLines = [], logLoading = false, logViewGen = 0;
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
    function clearLogUI() { logLines = []; logViewGen++; document.getElementById('logView').textContent = ''; }
    function refreshLog() {
      if (document.hidden) return;           // 后台标签页不轮询，省带宽与设备负载
      if (logLoading) return;                // 防止手动刷新/自动轮询并发，把同一批日志拼两遍
      var el = document.getElementById('logView');
      var reqSince = logSince, reqGen = logViewGen;
      logLoading = true;
      fetch('/log?since=' + reqSince).then(function(r) { return r.json(); }).then(function(d) {
        if (reqGen !== logViewGen) return;   // 清空显示后返回的旧响应直接丢弃
        if (!d || !Array.isArray(d.lines)) return;
        if (d.seq < logSince) logLines = [];  // 序号回退(设备重启) -> 重置
        if (reqSince !== logSince && d.seq <= logSince) return;  // 过期响应，不重复追加
        logSince = d.seq;
        if (d.lines.length) {
          logLines = logLines.concat(d.lines);
          if (logLines.length > 500) logLines = logLines.slice(logLines.length - 500);
          el.textContent = logLines.join('\n');
          el.scrollTop = el.scrollHeight;
        } else if (el.textContent === '加载中...') {
          el.textContent = '(暂无日志)';
        }
      }).catch(function() {
        if (el.textContent === '加载中...') el.textContent = '无法获取日志';
      }).finally(function() {
        logLoading = false;
      });
    }
    // ---- Keep-Alive (保号) ----
    function kaLoadStatus() {
      fetch('/keepalive?action=status').then(function(r){return r.json();}).then(function(d) {
        document.getElementById('kaEnabled').checked = !!d.enabled;
        document.getElementById('kaIntervalDays').value = d.intervalDays;
        document.getElementById('kaAction').value = d.action;
        document.getElementById('kaTarget').value = d.target || '';
        var cd = document.getElementById('kaCountdown');
        if (!d.timeValid) cd.textContent = '时间未同步，倒计时暂不可用';
        else if (!d.lastTime) cd.textContent = '尚未建立基准日，启用后首次检查时建立';
        else cd.textContent = '距下次保号约 ' + d.daysLeft + ' 天';
      }).catch(function(){});
    }
    var kaRunTimer = null;
    function kaPollRunStatus() {
      if (kaRunTimer) clearTimeout(kaRunTimer);
      fetch('/keepalive?action=status').then(function(r){return r.json();}).then(function(d) {
        var rbox = document.getElementById('kaResult');
        if (d.jobQueued || d.jobRunning) {
          rbox.className = 'result-box result-loading';
          rbox.textContent = d.jobMessage || '保号动作后台执行中...';
          kaRunTimer = setTimeout(kaPollRunStatus, 1500);
          return;
        }
        if (d.jobDone) {
          rbox.className = 'result-box ' + (d.jobSuccess ? 'result-success' : 'result-error');
          rbox.textContent = d.jobMessage || (d.jobSuccess ? '保号动作已完成' : '保号动作失败');
          kaLoadStatus();
        }
      }).catch(function(e){
        var rbox = document.getElementById('kaResult');
        rbox.className = 'result-box result-error';
        rbox.textContent = '状态查询失败: ' + e;
      });
    }
    function kaRun() {
      var r = document.getElementById('kaResult');
      r.className = 'result-box result-loading'; r.textContent = '正在提交后台保号任务...';
      fetch('/keepalive?action=run').then(function(x){return x.json();}).then(function(d) {
        if (d.success && d.queued) {
          r.textContent = d.message || '保号动作已排队';
          kaPollRunStatus();
        } else {
          r.className = 'result-box result-error';
          r.textContent = d.message || '任务启动失败';
        }
      }).catch(function(e){ r.className = 'result-box result-error'; r.textContent = '请求失败: ' + e; });
    }
    function kaReset() {
      if (!confirm('确定把保号基准日重置为今天？')) return;
      var r = document.getElementById('kaResult');
      r.className = 'result-box result-loading'; r.textContent = '处理中...';
      fetch('/keepalive?action=reset').then(function(x){return x.json();}).then(function(d) {
        r.className = 'result-box result-success'; r.textContent = d.message; kaLoadStatus();
      }).catch(function(e){ r.className = 'result-box result-error'; r.textContent = '请求失败: ' + e; });
    }

    // ---- Overview dashboard (live /status) ----
    function ovSet(id, v) { var e = document.getElementById(id); if (e) e.textContent = v; }
    function fmtCsq(c) {
      if (c == null || c >= 99 || c < 0) return '无信号';
      var dbm = -113 + 2 * c;
      var q = c >= 19 ? '优秀' : c >= 14 ? '良好' : c >= 10 ? '一般' : c >= 5 ? '较差' : '很差';
      return dbm + ' dBm ' + q;
    }
    // WiFi RSSI 质量分级，与诊断"WiFi 状态"查询一致
    function fmtWifiRssi(r) {
      if (r == null || r >= 0 || r < -200) return '--';
      var q = r >= -50 ? '信号极好' : r >= -60 ? '信号很好' : r >= -70 ? '信号良好' : r >= -80 ? '信号一般' : r >= -90 ? '信号较弱' : '信号很差';
      return r + ' dBm (' + q + ')';
    }
    var devTz = 480;  // 设备时区分钟偏移(从 /status 更新)，按此格式化时间，与查看者所在时区无关
    var apHandled = false;  // 配网模式只自动跳转一次
    function fmtEpoch(ep) {
      if (!ep) return '(无)';
      var d = new Date((ep + devTz * 60) * 1000), p = function(n){ return ('0' + n).slice(-2); };
      return d.getUTCFullYear() + '-' + p(d.getUTCMonth() + 1) + '-' + p(d.getUTCDate()) + ' ' + p(d.getUTCHours()) + ':' + p(d.getUTCMinutes());
    }
    function fmtClockEpoch(ep) {
      if (!ep) return '--';
      var d = new Date((ep + devTz * 60) * 1000), p = function(n){ return ('0' + n).slice(-2); };
      return p(d.getUTCHours()) + ':' + p(d.getUTCMinutes()) + ':' + p(d.getUTCSeconds());
    }
    function fmtRsrp(r) { if (r == null || r >= 0 || r < -200) return '--'; return r + ' dBm'; }
    function fmtBer(b) { return (b == null || b >= 99) ? '99 (未知)' : String(b); }
    function fmtDb(v, unit) { if (v == null || v === 999 || v < -200) return '--'; return v + (unit || ' dB'); }
    function fmtUptime(s) { var h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), x = s % 60; return h + ':' + ('0' + m).slice(-2) + ':' + ('0' + x).slice(-2); }
    // ESP-IDF esp_reset_reason_t 显示名；11=USB 外设复位，常见于 USB 串口/下载/主机重新枚举。
    function resetReasonText(v) {
      var m = {0:'未知',1:'上电/复位键',2:'外部引脚',3:'软件重启',4:'异常崩溃',5:'中断看门狗',6:'任务看门狗',7:'其他看门狗',8:'深睡眠唤醒',9:'棕断/欠压',10:'SDIO复位',11:'USB复位',12:'JTAG复位',13:'eFuse错误',14:'电源毛刺',15:'CPU锁死'};
      return (m[v] || '原因码') + ' (' + v + ')';
    }
    function kb(b) { return Math.round(b / 1024) + ' KB'; }
    function setDot(id, lvl) { var e = document.getElementById(id); if (e) e.className = 'dot ' + lvl; }
    function sigLevel(c) { if (c == null || c >= 99 || c < 0) return 'bad'; return c >= 14 ? 'ok' : c >= 8 ? 'warn' : 'bad'; }
    // 信号量表填充：raw 映射到 lo..hi 百分比，按 warnAt/okAt 染色(越大越好)
    function setGauge(id, raw, lo, hi, label, warnAt, okAt) {
      var fill = document.getElementById(id), vEl = document.getElementById(id + 'V');
      if (vEl) vEl.textContent = label;
      if (!fill) return;
      var bad = (raw == null || raw === 999 || raw < -200 || (lo === 0 && hi === 31 && raw >= 99));
      if (bad) { fill.style.width = '0'; fill.className = 'fill'; return; }
      var pct = Math.max(2, Math.min(100, (raw - lo) / (hi - lo) * 100));
      fill.style.width = pct + '%';
      fill.className = 'fill ' + (raw >= okAt ? 'ok' : raw >= warnAt ? 'warn' : 'bad');
    }
    var statusTimer = null, statusPolling = false, statusSeq = 0, statusAbort = null, statusLoading = false, statusFailCount = 0, devEpochBase = 0, devEpochBaseMs = 0, latestStatusKey = '', statusFastUntil = 0;
    function deviceEpochNow() {
      if (!devEpochBase) return 0;
      return devEpochBase + Math.floor((Date.now() - devEpochBaseMs) / 1000);
    }
    function updateDeviceClockLabel() {
      var ep = deviceEpochNow();
      if (ep) ovSet('ovRefresh', '设备 ' + fmtClockEpoch(ep));
    }
    function loadStatus() {
      if (statusLoading) return;  // 上一次 /status 还没回来时不叠加请求，避免 ESP32 被轮询拖慢
      statusLoading = true;
      var seq = ++statusSeq, timedOut = false, finished = false;
      var ctrl = window.AbortController ? new AbortController() : null;
      statusAbort = ctrl;
      var opt = { cache: 'no-store' };
      if (ctrl) opt.signal = ctrl.signal;
      var timeoutId = setTimeout(function() {
        if (finished || seq !== statusSeq) return;
        timedOut = true;
        if (ctrl) ctrl.abort();
        else ovSet('ovRefresh', '刷新超时');
      }, 7000);
      fetch('/status?_=' + Date.now(), opt).then(function(r){return r.json();}).then(function(d) {
        if (seq !== statusSeq || timedOut) return;  // 旧响应直接丢弃，只显示最新状态
        statusFailCount = 0;
        if (typeof d.tz === 'number') devTz = d.tz;
        if (typeof d.nowEpoch === 'number' && d.nowEpoch > 100000) {
          devEpochBase = d.nowEpoch;
          devEpochBaseMs = Date.now();
          updateDeviceClockLabel();
        }
        // KPI 指标条
        setGauge('gCsq', d.csq, 0, 31, (d.csq == null || d.csq >= 99) ? '无信号' : (d.csq + ' /31'), 8, 14);  // CSQ 原始值
        setGauge('gRssi', (d.csq == null || d.csq >= 99) ? null : (-113 + 2 * d.csq), -110, -50, fmtCsq(d.csq), -95, -75);  // RSSI=CSQ 换算 dBm(与诊断"信号查询"一致)
        setGauge('gRsrp', d.rsrp, -120, -70, fmtRsrp(d.rsrp), -110, -100);
        setGauge('gRsrq', d.rsrq, -20, -3, fmtDb(d.rsrq), -15, -10);
        setGauge('gSinr', d.sinr, 0, 30, fmtDb(d.sinr), 0, 13);
        setGauge('gWifi', (d.rssi == null || d.rssi >= 0) ? null : d.rssi, -90, -40, (d.rssi == null || d.rssi >= 0) ? '--' : (d.rssi + ' dBm'), -75, -65);  // WiFi RSSI(质量见下方卡片)
        setDot('dotSig', sigLevel(d.csq));
        ovSet('ovData', d.dataEnabled ? '已启用' : '已禁用');
        ovSet('ovDataSub', d.dataEnabled ? (d.cellIp || '获取中') : '零流量');
        setDot('dotData', d.dataEnabled ? 'warn' : 'ok');   // 禁用=安全(绿)，启用=在用流量(橙)
        ovSet('ovSms', d.smsTotal); ovSet('ovLastSms', '最近 ' + fmtEpoch(d.lastSmsEpoch));
        ovSet('ovInbox', d.inboxCount + ' 条');
        var busyQueues = !!(d.slowBusy || d.fwdQueueDepth || d.queueDepth || d.outSmsQueueDepth || d.emailQueueDepth);
        if (busyQueues) statusFastUntil = Date.now() + 12000;
        var lk = [d.smsTotal, d.lastSmsEpoch, d.inboxCount, d.fwdQueueDepth, d.queueDepth, d.slowBusy ? 1 : 0].join(':');
        if (lk !== latestStatusKey) {
          latestStatusKey = lk;
          loadLatestOtp();
        }
        // 设备 / 固件信息卡片
        ovSet('dvHeap', kb(d.freeHeap) + ' · 最低 ' + kb(d.minFreeHeap));
        window.__upt = d.uptime; ovSet('dvUptime', fmtUptime(d.uptime)); ovSet('dvEspVer', d.version);
        ovSet('dvTemp', (d.chipTemp != null ? d.chipTemp + ' ℃' : '--'));
        ovSet('dvMfr', d.mfr || '--'); ovSet('dvModel', d.model || '--'); ovSet('dvFw', d.fwver || '--');
        // SIM 卡信息
        ovSet('tOp', d.operator || '--'); ovSet('tModem', d.modemReady ? '已就绪' : '未就绪');
        ovSet('tCellIp', d.dataEnabled ? (d.cellIp || '获取中') : '— (未启用)');
        ovSet('tPhone', d.phone || '--'); ovSet('tImei', d.imei || '--'); ovSet('tIccid', d.iccid || '--');
        ovSet('tImsi', d.imsi || '--'); ovSet('tApn', d.apnSim || d.apn || '--');
        // WiFi 详细信息卡片(独立卡，原"网络与SIM"里的 WiFi 行已移出)
        ovSet('wfSsid', d.ssid || '--');
        ovSet('wfIp', d.ip || '--'); ovSet('wfGw', d.gw || '--'); ovSet('wfMask', d.mask || '--');
        ovSet('wfDns', d.dns || '--'); ovSet('wfMac', d.mac || '--'); ovSet('wfBssid', d.bssid || '--');
        ovSet('wfChan', (d.chan != null && d.chan > 0) ? d.chan : '--');
        setDot('dotWifi', d.wifiConnected ? 'ok' : 'bad');
        // 转发与系统
        var qText = d.fwdQueueDepth + ' / ' + d.queueDepth + ' / ' + (d.outSmsQueueDepth || 0) + ' / ' + (d.emailQueueDepth || 0);
        if (d.slowBusy) qText += ' · 推送中';
        ovSet('tQueue', qText);
        ovSet('tMaxBlock', kb(d.maxAllocHeap));
        ovSet('tReset', resetReasonText(d.resetReason));
        ovSet('tTime', (d.timeSynced ? '已同步 ' : '未同步 ') + (d.nowEpoch > 100000 ? fmtEpoch(d.nowEpoch) : '--'));
        if (d.apMode) {
          var lv = document.getElementById('ovLive');
          if (lv) { lv.textContent = '● 配网模式（请到 WiFi 设置配置网络）'; lv.style.color = 'var(--error)'; }
          if (!apHandled) { apHandled = true; switchPanel('settings'); }
        }
      }).catch(function() {
        if (seq !== statusSeq) return;
        statusFailCount++;
        if (timedOut) { ovSet('ovRefresh', '刷新超时'); return; }
        ovSet('ovRefresh', '刷新失败');
        if (statusFailCount >= 3) {
          var e = document.getElementById('ovLive'); if (e) { e.textContent = '● 离线'; e.style.color = 'var(--error)'; }
        }
      }).then(function() {
        if (seq !== statusSeq) return;
        finished = true;
        statusLoading = false;
        clearTimeout(timeoutId);
        if (statusAbort === ctrl) statusAbort = null;
      });
    }
    function scheduleStatusPoll(delay) {
      if (!statusPolling || statusTimer) return;
      statusTimer = setTimeout(function() {
        statusTimer = null;
        if (!statusPolling) return;
        if (!document.hidden) loadStatus();
        scheduleStatusPoll(Date.now() < statusFastUntil ? 800 : 2000);
      }, delay);
    }
    function startStatusPoll() {
      if (statusPolling) return;
      statusPolling = true;
      scheduleStatusPoll(500);
      // 运行时长本地秒表：每秒自增显示(不靠轮询)，由 loadStatus 拉到的 uptime 校准
      if (!window.__uptTimer) window.__uptTimer = setInterval(function(){
        if (window.__upt != null && !document.hidden) { window.__upt++; var e = document.getElementById('dvUptime'); if (e) e.textContent = fmtUptime(window.__upt); }
        if (!document.hidden) updateDeviceClockLabel();
      }, 1000);
    }
    function stopStatusPoll() { statusPolling = false; if (statusTimer) { clearTimeout(statusTimer); statusTimer = null; } }

    // ---- SIM / 网络 ----
    // ---- SMS (收 / 发) ----
    var smsBox = 'recv';
    function smsTab(b) {
      smsBox = b;
      document.getElementById('tabRecv').classList.toggle('active', b === 'recv');
      document.getElementById('tabSent').classList.toggle('active', b === 'sent');
      loadMessages();
    }
    // 验证码提取(镜像固件 extractOtp：首段 4-8 位独立数字串)
    function otpExtract(t) { var m = String(t).match(/(?:^|\D)(\d{4,8})(?:\D|$)/); return m ? m[1] : ''; }
    function htmlEsc(s) { return String(s).replace(/[&<>"]/g, function(c){ return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]; }); }
    function copyText(txt, el) {
      try { if (navigator.clipboard) navigator.clipboard.writeText(txt); } catch (e) {}
      if (el) { var o = el.textContent; el.textContent = '已复制'; setTimeout(function(){ el.textContent = o; }, 800); }
    }
    function loadMessages() {
      var box = document.getElementById('inboxList');
      var sent = (smsBox === 'sent');
      // 有同类缓存先即时渲染(避免每次进入都空白"加载中")，再后台拉取刷新
      if (window.__msgFull && window.__msgBox === (sent ? 'sent' : 'recv')) renderMessages();
      else box.textContent = '加载中...';
      fetch('/messages' + (sent ? '?box=sent' : '')).then(function(r){return r.json();}).then(function(arr) {
        if (!Array.isArray(arr)) arr = [];
        window.__msgFull = arr;                 // 全量缓存：搜索本地过滤，不再每按键请求
        window.__msgBox = sent ? 'sent' : 'recv';
        renderMessages();
      }).catch(function() { box.textContent = '无法获取短信'; });
    }
    // 仅用缓存过滤+渲染(搜索框输入调用，零网络请求，瞬时响应)
    function renderMessages() {
      var box = document.getElementById('inboxList');
      var sent = (window.__msgBox === 'sent');
      var qEl = document.getElementById('smsSearch');
      var q = (qEl && qEl.value || '').trim().toLowerCase();
      var arr = (window.__msgFull || []).slice();
      if (q) arr = arr.filter(function(m){ var who = sent ? (m.target||'') : (m.sender||''); return (who + ' ' + (m.text||'')).toLowerCase().indexOf(q) >= 0; });
      box.innerHTML = '';
      if (!arr.length) {
        var em = document.createElement('div'); em.className = 'msg-empty';
        em.textContent = q ? ('未匹配 “' + q + '”') : (sent ? '(暂无已发送短信)' : '(暂无短信)'); box.appendChild(em); return;
      }
      window.__msgCache = arr;
      arr.forEach(function(m) {
          var d = document.createElement('div'); d.className = 'msg'; d.style.cursor = 'pointer'; d.onclick = function(){ openMsgDrawer(m.id); };
          var h = document.createElement('div'); h.className = 'msg-head';
          var s = document.createElement('span'); s.className = 'msg-sender';
          var right = document.createElement('span'); right.style.display = 'flex'; right.style.gap = '6px'; right.style.alignItems = 'center';
          var otp = sent ? '' : otpExtract(m.text || '');
          if (otp) {
            var oc = document.createElement('span'); oc.className = 'msg-chip otp'; oc.textContent = '⎘ ' + otp; oc.title = '点击复制验证码';
            oc.onclick = function(ev){ ev.stopPropagation(); copyText(otp, oc); };
            right.appendChild(oc);
          }
          var chip = document.createElement('span'); chip.className = 'msg-chip';
          if (sent) {
            s.textContent = m.target || '(未知)';
            chip.textContent = m.ok ? '发送成功' : '发送失败'; chip.className += m.ok ? ' ok' : ' bad';
          } else {
            s.textContent = m.sender || '(未知)';
            chip.textContent = m.fwd ? '已处理' : '待处理'; chip.className += m.fwd ? ' ok' : ' wait';
          }
          right.appendChild(chip);
          h.appendChild(s); h.appendChild(right);
          var t = document.createElement('div'); t.className = 'msg-time';
          t.textContent = fmtEpoch(sent ? m.sent : m.recv);
          var b = document.createElement('div'); b.className = 'msg-body';
          var body = htmlEsc(m.text || '');
          if (otp) body = body.replace(otp, '<mark>' + otp + '</mark>');  // otp 为纯数字，先转义再高亮，无注入风险
          b.innerHTML = body;
          d.appendChild(h); d.appendChild(t); d.appendChild(b); box.appendChild(d);
        });
    }

    // ---- System maintenance ----
    function sysReboot() {
      if (!confirm('确定重启设备？约 15 秒后恢复。')) return;
      var r = document.getElementById('sysResult');
      r.className = 'result-box result-loading'; r.textContent = '正在重启...';
      fetch('/reboot', {method:'POST'}).then(function(x){return x.json();}).then(function(d){
        r.className = 'result-box result-success'; r.textContent = d.message;
      }).catch(function(){ r.className = 'result-box result-info'; r.textContent = '设备正在重启，请稍后刷新页面'; });
    }
    function sysFactory() {
      if (!confirm('确定恢复出厂设置？将清空所有配置并重启，不可撤销！')) return;
      if (!confirm('再次确认：账号/邮件/推送/保号等全部配置都会被清除！')) return;
      var r = document.getElementById('sysResult');
      r.className = 'result-box result-loading'; r.textContent = '正在清除配置并重启...';
      fetch('/factory', {method:'POST'}).then(function(x){return x.json();}).then(function(d){
        r.className = 'result-box result-success'; r.textContent = d.message;
      }).catch(function(){ r.className = 'result-box result-info'; r.textContent = '设备正在重启，请稍后用默认账号登录'; });
    }

    // ---- WiFi settings ----
    function wifiScan() {
      var sel = document.getElementById('wifiScanSel');
      sel.innerHTML = '<option value="">扫描中（约 3 秒）...</option>';
      fetch('/wifiscan').then(function(r){return r.json();}).then(function(arr) {
        if (!Array.isArray(arr) || !arr.length) { sel.innerHTML = '<option value="">未发现 WiFi</option>'; return; }
        arr.sort(function(a, b){ return b.rssi - a.rssi; });
        sel.innerHTML = '<option value="">选择网络（共 ' + arr.length + ' 个）</option>';
        arr.forEach(function(w) {
          var o = document.createElement('option');
          o.value = w.ssid;
          o.textContent = (w.ssid || '(隐藏网络)') + '  ·  ' + w.rssi + ' dBm  ·  ' + (w.enc ? '加密' : '开放');
          sel.appendChild(o);
        });
      }).catch(function(){ sel.innerHTML = '<option value="">扫描失败</option>'; });
    }
    function wifiPick() {
      var v = document.getElementById('wifiScanSel').value;
      if (v) { document.getElementById('wifiSsidIn').value = v; document.getElementById('wifiPassIn').focus(); }
    }
    function wifiSave() {
      var s = document.getElementById('wifiSsidIn').value.trim();
      if (!s) { alert('请输入 WiFi 名称'); return; }
      if (!confirm('保存并重启接入 “' + s + '”？设备将重启约 15-20 秒。')) return;
      var r = document.getElementById('wifiCfgResult');
      r.className = 'result-box result-loading'; r.textContent = '保存中，设备即将重启...';
      var body = 'ssid=' + encodeURIComponent(s) + '&pass=' + encodeURIComponent(document.getElementById('wifiPassIn').value);
      fetch('/wificonfig', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:body}).then(function(x){return x.json();}).then(function(d) {
        r.className = 'result-box ' + (d.success ? 'result-success' : 'result-error'); r.textContent = d.message;
      }).catch(function(){ r.className = 'result-box result-info'; r.textContent = '设备正在重启，请连接目标 WiFi 后重新访问设备'; });
    }
    function wifiPrefill() {
      fetch('/status').then(function(r){return r.json();}).then(function(d) {
        if (d.ssid && !d.apMode && !document.getElementById('wifiSsidIn').value) document.getElementById('wifiSsidIn').value = d.ssid;
      }).catch(function(){});
    }

    // ---- Config backup / OTA ----
    function doExport() { location.href = '/export'; }
    function doImport() {
      var t = document.getElementById('importBox').value;
      if (!t.trim()) { return; }
      if (!confirm('确定导入配置？将覆盖当前设置。')) return;
      var r = document.getElementById('importResult');
      r.className = 'result-box result-loading'; r.textContent = '导入中...';
      fetch('/import', {method:'POST', headers:{'Content-Type':'text/plain'}, body:t}).then(function(x){return x.json();}).then(function(d){
        r.className = 'result-box ' + (d.success ? 'result-success' : 'result-error'); r.textContent = d.message;
      }).catch(function(e){ r.className = 'result-box result-error'; r.textContent = '请求失败: ' + e; });
    }
    function doOta() {
      var f = document.getElementById('otaFile').files[0];
      if (!f) { alert('请先选择 .bin 固件文件'); return; }
      if (!confirm('确定升级固件？升级过程中请勿断电。')) return;
      var r = document.getElementById('otaResult');
      r.className = 'result-box result-loading'; r.textContent = '上传中，请勿断电...';
      var fd = new FormData(); fd.append('update', f, f.name);
      fetch('/update', {method:'POST', body:fd}).then(function(x){return x.json();}).then(function(d){
        r.className = 'result-box ' + (d.success ? 'result-success' : 'result-error'); r.textContent = d.message;
      }).catch(function(){ r.className = 'result-box result-info'; r.textContent = '设备可能正在重启，请稍后刷新'; });
    }

    // ---- 转发规则 可视化搭建器 ----
    var fwdRules = [];  // {type,pattern,chans{},email,drop,enabled}
    function parseFwdRules() {
      var raw = (document.getElementById('forwardRulesRaw') || {}).value || '';
      fwdRules = [];
      raw.split('\n').forEach(function(line) {
        if (!line.trim()) return;
        var p = line.split('\t'); if (p.length < 3) return;
        var r = { type: p[0], pattern: p[1], chans: {}, email: false, drop: false, enabled: (p.length > 3 ? p[3] : '1') !== '0' };
        (p[2] || '').split(',').forEach(function(t) { t = t.trim();
          if (t === 'drop') r.drop = true; else if (t === 'email') r.email = true; else if (/^[1-5]$/.test(t)) r.chans[t] = true; });
        fwdRules.push(r);
      });
    }
    function serializeRules() {
      var lines = fwdRules.map(function(r) {
        var a = []; if (r.drop) a.push('drop'); else { if (r.email) a.push('email'); for (var c = 1; c <= 5; c++) if (r.chans[c]) a.push(String(c)); }
        return r.type + '\t' + r.pattern + '\t' + a.join(',') + '\t' + (r.enabled ? '1' : '0');
      });
      var el = document.getElementById('forwardRulesRaw'); if (el) el.value = lines.join('\n');
    }
    function addRule() { fwdRules.push({ type: 'kw', pattern: '', chans: {}, email: true, drop: false, enabled: true }); renderRules(); }
    function delRule(i) { fwdRules.splice(i, 1); renderRules(); }
    function moveRule(i, dlt) { var j = i + dlt; if (j < 0 || j >= fwdRules.length) return; var t = fwdRules[i]; fwdRules[i] = fwdRules[j]; fwdRules[j] = t; renderRules(); }
    function renderRules() {
      serializeRules();
      var box = document.getElementById('rulesList'); if (!box) return;
      if (!fwdRules.length) { box.innerHTML = '<div class="msg-empty">暂无规则。点“添加规则”新建；不加规则时短信转发到全部启用通道。</div>'; return; }
      box.innerHTML = '';
      fwdRules.forEach(function(r, i) {
        var d = document.createElement('div'); d.className = 'push-channel enabled'; d.style.marginBottom = '10px';
        var typeOpts = [['kw','关键词(子串)'],['re','正文正则'],['from','发件人正则']].map(function(o){return '<option value="'+o[0]+'"'+(r.type===o[0]?' selected':'')+'>'+o[1]+'</option>';}).join('');
        var chans = ''; for (var c = 1; c <= 5; c++) chans += '<label style="margin-right:12px;font-size:12px;white-space:nowrap;"><input type="checkbox" data-ch="'+c+'"'+(r.chans[c]?' checked':'')+(r.drop?' disabled':'')+'> 通道'+c+'</label>';
        d.innerHTML =
          '<div class="push-channel-header"><b style="font-family:var(--mono);color:var(--amber);">'+(i+1<10?'0':'')+(i+1)+'</b>'+
          '<label style="font-size:12px;"><input type="checkbox" class="rEn"'+(r.enabled?' checked':'')+'> 启用</label>'+
          '<span style="margin-left:auto;display:flex;gap:6px;">'+
          '<button type="button" class="btn btn-secondary btn-sm" data-a="up">↑</button>'+
          '<button type="button" class="btn btn-secondary btn-sm" data-a="dn">↓</button>'+
          '<button type="button" class="btn btn-danger btn-sm" data-a="del">删除</button></span></div>'+
          '<div class="form-row"><div class="form-group" style="flex:0 0 132px;"><label class="form-label">匹配方式</label><select class="form-select rType">'+typeOpts+'</select></div>'+
          '<div class="form-group"><label class="form-label">模式（关键词 / 正则）</label><input class="form-input rPat" placeholder="如  验证码   或   ^(10086|10010)"></div></div>'+
          '<div class="form-group"><label class="form-label">命中后</label>'+
          '<label style="margin-right:14px;font-size:12px;color:var(--error);"><input type="checkbox" class="rDrop"'+(r.drop?' checked':'')+'> 丢弃(不转发)</label>'+
          '<label style="margin-right:12px;font-size:12px;"><input type="checkbox" class="rEmail"'+(r.email?' checked':'')+(r.drop?' disabled':'')+'> 邮件</label>'+chans+'</div>';
        d.querySelector('.rType').onchange = function(){ r.type = this.value; serializeRules(); };
        var pat = d.querySelector('.rPat'); pat.value = r.pattern; pat.oninput = function(){ r.pattern = this.value; serializeRules(); };
        d.querySelector('.rEn').onchange = function(){ r.enabled = this.checked; serializeRules(); };
        d.querySelector('.rDrop').onchange = function(){ r.drop = this.checked; renderRules(); };
        d.querySelector('.rEmail').onchange = function(){ r.email = this.checked; serializeRules(); };
        [].forEach.call(d.querySelectorAll('input[data-ch]'), function(cb){ cb.onchange = function(){ r.chans[this.getAttribute('data-ch')] = this.checked; serializeRules(); }; });
        d.querySelector('[data-a=up]').onclick = function(){ moveRule(i, -1); };
        d.querySelector('[data-a=dn]').onclick = function(){ moveRule(i, 1); };
        d.querySelector('[data-a=del]').onclick = function(){ delRule(i); };
        box.appendChild(d);
      });
    }

    // ---- 短信详情抽屉(点击展开) ----
    function openMsgDrawer(id) {
      var arr = window.__msgCache || [], box = window.__msgBox || 'recv', m = null;
      for (var i = 0; i < arr.length; i++) if (arr[i].id === id) { m = arr[i]; break; }
      if (!m) return;
      var sent = (box === 'sent');
      var who = sent ? (m.target || '(未知)') : (m.sender || '(未知)');
      var otp = sent ? '' : otpExtract(m.text || '');
      var h = '<div class="dk">' + (sent ? '目标' : '发件人') + '</div><div class="dv">' + htmlEsc(who) + '</div>';
      h += '<div class="dk">时间</div><div class="dv">' + fmtEpoch(sent ? m.sent : m.recv) + '</div>';
      h += '<div class="dk">状态</div><div class="dv">' + (sent ? (m.ok ? '发送成功' : '发送失败') : (m.fwd ? '已处理' : '待处理')) + '</div>';
      if (otp) h += '<div class="dk">验证码</div><div class="dv"><span class="otpcode" onclick="copyText(\'' + otp + '\', this)">' + otp + '</span></div>';
      var bh = htmlEsc(m.text || ''); if (otp) bh = bh.replace(otp, '<mark>' + otp + '</mark>');
      h += '<div class="dfull">' + bh + '</div><div class="btn-row">';
      if (!sent) h += '<button class="btn btn-primary btn-sm" onclick="resendMsg(' + id + ')">重发转发</button>';
      if (!sent) h += '<button class="btn btn-danger btn-sm" onclick="deleteMsg(' + id + ')">删除</button>';
      h += '<button class="btn btn-secondary btn-sm" id="drawerCopyFull">复制全文</button></div><div class="result-box" id="drawerRes"></div>';
      document.getElementById('drawerBody').innerHTML = h;
      var cf = document.getElementById('drawerCopyFull'); if (cf) cf.onclick = function(){ copyText(m.text || '', cf); };
      document.getElementById('msgDrawer').classList.add('show');
      document.getElementById('drawerBg').classList.add('show');
    }
    function closeMsgDrawer() { document.getElementById('msgDrawer').classList.remove('show'); document.getElementById('drawerBg').classList.remove('show'); }
    function resendMsg(id) {
      var r = document.getElementById('drawerRes'); r.className = 'result-box result-loading'; r.textContent = '重发中...';
      fetch('/resend?id=' + id, { method: 'POST' }).then(function(x){return x.json();}).then(function(d) {
        r.className = 'result-box ' + (d.success ? 'result-success' : 'result-error'); r.textContent = d.message;
      }).catch(function(e){ r.className = 'result-box result-error'; r.textContent = '请求失败: ' + e; });
    }
    function deleteMsg(id) {
      if (!confirm('确定删除这条短信？')) return;
      fetch('/delete?id=' + id, { method: 'POST' }).then(function(x){return x.json();}).then(function(d) {
        if (d.success) { closeMsgDrawer(); loadMessages(); }
        else { var r = document.getElementById('drawerRes'); r.className = 'result-box result-error'; r.textContent = d.message; }
      }).catch(function(){});
    }
    // ---- 概览：最新接收 / 验证码 hero ----
    var latestOtpLoading = false;
    function loadLatestOtp() {
      if (latestOtpLoading) return;
      latestOtpLoading = true;
      fetch('/messages?limit=1').then(function(r){return r.json();}).then(function(arr) {
        var card = document.getElementById('otpHeroCard');
        if (!card) return;
        if (!Array.isArray(arr) || !arr.length) { card.style.display = 'none'; return; }
        var m = arr[0], otp = otpExtract(m.text || '');
        document.getElementById('ohFrom').textContent = m.sender || '(未知)';
        document.getElementById('ohText').textContent = m.text || '';
        document.getElementById('ohWhen').textContent = fmtEpoch(m.recv) + (m.fwd ? '　已处理' : '　待处理');
        var code = document.getElementById('ohCode');
        if (otp) { code.textContent = otp; code.onclick = function(){ copyText(otp, code); }; code.style.display = ''; } else { code.style.display = 'none'; }
        card.style.display = '';
        if (!m.fwd) setTimeout(loadLatestOtp, 1500);  // 刚入库时转发拆分可能下一帧才标记完成
      }).catch(function(){}).then(function(){ latestOtpLoading = false; });
    }

    var _origSwitchPanel = switchPanel;
    switchPanel = function(name) {
      _origSwitchPanel(name);
      if (name === 'log') { refreshLog(); startLogPoll(); } else { stopLogPoll(); }   // 仅日志面板轮询
      if (name === 'overview') { loadStatus(); loadLatestOtp(); startStatusPoll(); } else { stopStatusPoll(); }
      if (name === 'inbox') loadMessages();
      if (name === 'keepalive') kaLoadStatus();
      if (name === 'sim') wifiPrefill();
      if (name === 'push') { parseFwdRules(); renderRules(); }
    };
    document.addEventListener('DOMContentLoaded', function() {
      setTimeout(function() { loadStatus(); loadLatestOtp(); startStatusPoll(); }, 250);
    });
    // ---- 设置表单 AJAX 原地保存：拦截所有 /save 表单，不再整页跳转 ----
    function showSaved(ok, msg) {
      var t = document.getElementById('saveToast');
      if (!t) { t = document.createElement('div'); t.id = 'saveToast'; document.body.appendChild(t); }
      t.textContent = msg || (ok ? '✓ 已保存' : '✗ 保存失败');
      t.className = 'save-toast ' + (ok ? 'ok' : 'err');
      void t.offsetWidth;                 // 触发重排以重启过渡动画
      t.classList.add('show');
      clearTimeout(window.__toastT);
      window.__toastT = setTimeout(function() { t.classList.remove('show'); }, 2200);
    }
    document.addEventListener('DOMContentLoaded', function() {
      document.querySelectorAll('form[action="/save"]').forEach(function(f) {
        f.addEventListener('submit', function(e) {
          e.preventDefault();             // 阻止整页跳转；规则表单在此处再同步一次，避免事件顺序差异
          if (f.querySelector('#forwardRulesRaw')) serializeRules();
          var fd = new FormData(f);
          var body = new URLSearchParams();                  // 关键：用 urlencoded 而非 FormData(multipart)——
          fd.forEach(function(v, k){ body.append(k, v); });  // ESP32 WebServer 对 multipart 大表单会卡/超时致"网络错误"
          body.append('ajax', '1');
          var passChanged = (fd.get('webPass') || '') !== '';
          var btn = f.querySelector('button[type="submit"]'), old = btn ? btn.textContent : '';
          if (btn) { btn.disabled = true; btn.textContent = '保存中…'; }
          var attempt = function(n) {
            return fetch('/save', { method: 'POST', body: body }).then(function(r) {
              showSaved(r.ok, r.ok ? (passChanged ? '✓ 已保存，账号密码已更新' : '✓ 已保存') : ('✗ 保存失败 (' + r.status + ')'));
            }).catch(function() {
              if (n > 0) return new Promise(function(res){ setTimeout(res, 700); }).then(function(){ return attempt(n - 1); });  // 连接抖动自动重试
              showSaved(false, '✗ 网络错误（已重试，请稍后再点保存）');
            });
          };
          var done = function() { if (btn) { btn.disabled = false; btn.textContent = old; } };
          attempt(1).then(done, done);
        });
      });
    });
  </script>
</body>
</html>
)rawliteral";
