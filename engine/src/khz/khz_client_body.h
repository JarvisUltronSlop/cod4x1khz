#pragma once

struct cgs_t;
struct DObj_s;
struct XModel;
struct XAnim_s;
struct snapshot_s;
struct clientState_s;

// Native base + accepted client attachments, including heads/helmets. Weapon
// models are omitted only after verifying that they cannot replace any native
// gameplay tag or ancestor bone. Descriptors retain stock parent/duplicate data.
bool KHZ_ClientBodyPrepare(const cgs_t* gameStatic, XAnim_s* animations, const snapshot_s* accepted);
bool KHZ_ClientBodyRefresh(const cgs_t* gameStatic, const snapshot_s* accepted);
void KHZ_ClientBodyRelease();
const DObj_s* KHZ_ClientBodyForClient(const clientState_s* client);
unsigned KHZ_ClientBodyCount();
const DObj_s* KHZ_ClientBodyAt(unsigned index);
bool KHZ_ClientBodyRootTags(const DObj_s* object);
const char* KHZ_ClientBodyFailureReason();
unsigned KHZ_ClientBodyStockScripts();
unsigned KHZ_ClientBodyStockCompositions();

