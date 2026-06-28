#include "inbox.h"
#include <time.h>

static InboxEntry inbox[INBOX_MAX];
static int inboxHead = 0;    // 下一个写入位置
static int inboxFilled = 0;  // 已填充条数(<=INBOX_MAX)
static uint32_t inboxSeq = 0;

uint32_t inboxAdd(const char* sender, const char* text, const char* ts) {
  InboxEntry& e = inbox[inboxHead];
  e.id = ++inboxSeq;
  e.recvEpoch = (uint32_t)time(nullptr);
  e.sender = sender;
  e.ts = ts;
  String t = text;
  if ((int)t.length() > INBOX_BODY_MAX) {
    t = t.substring(0, INBOX_BODY_MAX);
    t += "…";
  }
  e.text = t;
  e.forwarded = false;
  e.deleted = false;
  inboxHead = (inboxHead + 1) % INBOX_MAX;
  if (inboxFilled < INBOX_MAX) inboxFilled++;
  return e.id;
}

void inboxMarkForwarded(uint32_t id) {
  if (id == 0) return;
  for (int i = 0; i < inboxFilled; i++) {
    if (inbox[i].id == id) { inbox[i].forwarded = true; return; }
  }
}

int inboxCount() {
  int c = 0;
  for (int k = 0; k < inboxFilled; k++) if (!inbox[k].deleted) c++;
  return c;
}

const InboxEntry* inboxAtNewest(int i) {
  if (i < 0) return nullptr;
  int seen = 0;
  for (int k = 0; k < inboxFilled; k++) {
    int idx = (inboxHead - 1 - k + INBOX_MAX * 2) % INBOX_MAX;  // 最新在前
    if (inbox[idx].deleted) continue;
    if (seen == i) return &inbox[idx];
    seen++;
  }
  return nullptr;
}

bool inboxDelete(uint32_t id) {
  if (id == 0) return false;
  for (int k = 0; k < inboxFilled; k++) {
    if (inbox[k].id == id && !inbox[k].deleted) {
      inbox[k].deleted = true;
      inbox[k].text = ""; inbox[k].sender = ""; inbox[k].ts = "";  // 释放堆
      return true;
    }
  }
  return false;
}

const InboxEntry* inboxById(uint32_t id) {
  if (id == 0) return nullptr;
  for (int k = 0; k < inboxFilled; k++) {
    if (inbox[k].id == id && !inbox[k].deleted) return &inbox[k];
  }
  return nullptr;
}

// ---- 已发送短信环形缓冲 ----
static SentEntry sent[SENT_MAX];
static int sentHead = 0;
static int sentFilled = 0;
static uint32_t sentSeq = 0;

void sentAdd(const char* target, const char* text, bool ok) {
  SentEntry& e = sent[sentHead];
  e.id = ++sentSeq;
  e.sentEpoch = (uint32_t)time(nullptr);
  e.target = target;
  String t = text;
  if ((int)t.length() > INBOX_BODY_MAX) { t = t.substring(0, INBOX_BODY_MAX); t += "…"; }
  e.text = t;
  e.ok = ok;
  sentHead = (sentHead + 1) % SENT_MAX;
  if (sentFilled < SENT_MAX) sentFilled++;
}

int sentCount() { return sentFilled; }

const SentEntry* sentAtNewest(int i) {
  if (i < 0 || i >= sentFilled) return nullptr;
  int idx = (sentHead - 1 - i + SENT_MAX * 2) % SENT_MAX;
  return &sent[idx];
}
