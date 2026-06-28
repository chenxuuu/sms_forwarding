#ifndef MODEM_H
#define MODEM_H

#include "globals.h"

String sendATCommand(const char* cmd, unsigned long timeout);
void modemPowerCycle();
void resetModule();
void modemInit();
void modemHealthTick();  // loop() 中按周期探测模组健康，连续失败自动断电恢复
bool queryModemImei(String& imeiOut, String* rawResp = nullptr, unsigned long timeout = 2000); // 查询一次 IMEI，按常见命令顺序回退
void sampleModemIdentity();  // 采样 IMEI / ICCID / 本机号码(开机一次)
void saveModemIdentityCache(); // 把已查询到的 IMEI/ICCID 写入 NVS 缓存，首页可立即显示
void sampleCellIp();         // 读取蜂窝 PDP IP(仅 dataEnabled)
void sampleSignalDetail();   // 采样 RSRP/RSRQ/SINR/PCI/PLMN/TAC(周期)
void signalSampleTick();     // loop() 周期调用：CSQ 高频/详情低频，与接收轮询解耦防长阻塞
void modemApplyDataMode();   // 按配置启/停蜂窝数据(SIM 页保存后即时生效)
void modemApplyOperator();   // 应用运营商选择(AT+COPS)
bool sendATandWaitOK(const char* cmd, unsigned long timeout);
bool waitCEREG();
void blink_short(unsigned long gap_time = 500);
bool sendSMS(const char* phoneNumber, const char* message);
bool enqueueOutgoingSms(const char* phoneNumber, const char* message);
void processOutgoingSmsQueue();
int outgoingSmsQueueDepth();
bool modemSerialTryBegin(const char* label);  // 自定义AT等待流程使用：进入独占串口区
void modemSerialEnd();                        // 自定义AT等待流程使用：退出独占串口区
bool consumeCellularDataBytes(unsigned long targetBytes,
                              const char* host = CELLULAR_BURN_DEFAULT_HOST,
                              uint16_t port = 53);  // PDP 已激活时发送 UDP 上行数据；内部硬限制不超过 CELLULAR_BURN_MAX_BYTES
bool consumeCellularViaHttpGet(const char* host = KEEPALIVE_HTTP_HOST);  // 保号: PDP 已激活时对 host 发起 HTTP GET 消耗下行流量(TCP/MIPOPEN)

#endif
