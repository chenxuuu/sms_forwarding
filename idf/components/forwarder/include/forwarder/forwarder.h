/**
 * @file forwarder.h
 * @brief 短信转发策略层：黑名单过滤、管理员命令、邮件/推送分发。
 *
 * 位于 modem（只负责收发）与 notify（只负责发出去）之间，是唯一的业务策略位置。
 */
#pragma once

/** 启动转发任务，并接管 modem 收到的完整短信。 */
void forwarder_init(void);

/** 发送带设备地址的通知邮件（启动通知、配置变更通知）。 */
void forwarder_notify_device(const char *title, const char *detail, const char *device_url);
