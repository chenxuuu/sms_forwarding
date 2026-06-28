#ifndef WEB_HANDLERS_H
#define WEB_HANDLERS_H

#include "globals.h"

#define LOG_BUF_SIZE 120   // 日志环形缓冲条数；120×~80B≈9.6KB(原 200 偏多，调小省 ~6KB 动态 RAM)

extern String logBuffer[LOG_BUF_SIZE];
extern int logBufIdx;
extern int logBufCount;

void logCapture(const String& msg);
void logCapture(const char* msg);
void logCaptureF(const char* fmt, ...);
void logCaptureLn(const String& msg);
void logCaptureLn(const char* msg);

bool checkAuth();
void handleRoot();
void handleToolsPage();
void handleSave();
void handleQuery();
void handleFlightMode();
void handleATCommand();
void handleSendSms();
void handlePing();
void processPingJob();    // 诊断 UDP 流量后台任务，避免 /ping HTTP handler 长时间阻塞网页刷新
void handleLog();
void handleModem();
void handleWifi();
void handleStatus();      // P1-7 机器可读健康状态 JSON
void handleMessages();    // 收件箱: 最近短信 JSON
void handleResend();      // 重发收件箱某条(/resend?id=)
void handleDeleteMsg();   // 删除收件箱某条(/delete?id=)
void handleReboot();      // 系统维护: 重启设备
void handleFactory();     // 系统维护: 恢复出厂设置
void handleExport();      // 配置导出
void handleImport();      // 配置导入
void handleLogDownload(); // 日志下载
void handleWifiScan();    // 扫描周边 WiFi
void handleWifiConfig();  // 保存 WiFi 并重启接入
void handleOtaUpload();   // OTA 上传(分块)
void handleOtaFinish();   // OTA 完成响应
void handleKeepAlive();   // E0 保号: status / run / reset
void handleTestPush();    // C2 通道发送测试
void handleUssd();        // C1 USSD 查询

#endif
