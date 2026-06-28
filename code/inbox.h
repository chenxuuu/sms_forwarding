#ifndef INBOX_H
#define INBOX_H

#include "globals.h"

// 本地收件箱：有界环形缓冲，保存最近收到的短信供网页查看(GET /messages)。
struct InboxEntry {
  uint32_t id;         // 单调递增 id(>0)
  uint32_t recvEpoch;  // 本地接收时间(Unix 秒)
  String sender;       // 发送者
  String ts;           // 短信自带时间戳
  String text;         // 正文(超过 INBOX_BODY_MAX 截断)
  bool forwarded;      // 是否已执行转发
  bool deleted;        // 是否已被网页删除(跳过计数/序列化)
};

uint32_t inboxAdd(const char* sender, const char* text, const char* ts);  // 返回该条 id
void inboxMarkForwarded(uint32_t id);
int inboxCount();
const InboxEntry* inboxAtNewest(int i);  // 时间倒序访问第 i 条；越界返回 nullptr
const InboxEntry* inboxById(uint32_t id);  // 按 id 查找(供重发)
bool inboxDelete(uint32_t id);             // 网页删除一条

// 已发送短信：有界环形缓冲(供网页“已发送”标签查看)
struct SentEntry {
  uint32_t id;
  uint32_t sentEpoch;  // 发送时间(Unix 秒)
  String target;       // 目标号码
  String text;         // 正文(截断到 INBOX_BODY_MAX)
  bool ok;             // 发送是否成功
};

void sentAdd(const char* target, const char* text, bool ok);
int sentCount();
const SentEntry* sentAtNewest(int i);

#endif
