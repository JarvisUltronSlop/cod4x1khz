#pragma once
// Main service/control owner only. Manifest fetch/verification runs on a
// private worker. Apply/rollback use the standalone stopped-process helper.
void KHZ_InitUpdateClientFeatures();
void KHZ_UpdateClientService();
void KHZ_ShutdownUpdateClientFeatures();
const char* KHZ_UpdateClientStatusJSON();
