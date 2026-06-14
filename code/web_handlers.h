#ifndef WEB_HANDLERS_H
#define WEB_HANDLERS_H

#include "globals.h"

bool checkAuth();
void handleRoot();
void handleToolsPage();
void handleSave();
void handleQuery();
void handleFlightMode();
void handleATCommand();
void handleSendSms();
void handlePing();

#endif
