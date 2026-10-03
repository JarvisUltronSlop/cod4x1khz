#include "khz_client_body.h"
#include <universal/q_shared.h>
#include <qcommon/threads.h>
#include <client_mp/client_mp.h>
#include <cgame_mp/cg_local_mp.h>
#include <bgame/bg_local.h>
#include <game_mp/g_public_mp.h>
#include <script/scr_const.h>
#include <script/scr_stringlist.h>
#include <xanim/xmodel.h>
#include <xanim/dobj.h>
#include <xanim/xanim.h>
#include <database/database.h>
#include <universal/q_parse.h>
#include <cstdio>
#include <cstring>

namespace {
constexpr unsigned BodyCapacity = 128, ModelCapacity = 7;
constexpr unsigned char UnknownBone = 254; // 255 is native's cached missing result.
struct Composition {
    unsigned count;
    XModel* models[ModelCapacity];
    unsigned short tags[ModelCapacity];
};
struct Body {
    Composition composition;
    DObj_s object;
    alignas(void*) unsigned char models[ModelCapacity * (sizeof(void*) + 1)];
    XAnimTree_s assetTree;
    unsigned validatedWeaponCount;
};
struct BodyDomain {
    bool prepared;
    unsigned count;
    XAnim_s* animations;
    Body bodies[BodyCapacity];
    XModel* modelPointers[512];
    bool modelNamed[512];
    unsigned short tagIds[CS_TAGS_COUNT];
    const char* failure;
    char failureDetail[1024];
    int diagnosticClient, diagnosticModel;
    unsigned stockScripts, stockCompositions;
};
BodyDomain bodyDomain; // loader-zeroed POD, no large CRT temporary

bool fail(const char* reason) { bodyDomain.failure = reason; return false; }
bool failDetail(const char* reason, const DObj_s* body, unsigned tag = 0) {
    auto& text = bodyDomain.failureDetail;
    std::snprintf(text, sizeof(text), "%s client=%d model_index=%d tag=%s", reason,
        bodyDomain.diagnosticClient, bodyDomain.diagnosticModel, tag ? SL_ConvertToString(tag) : "none");
    if (body) for (unsigned i = 0; i < body->numModels; ++i) {
        const auto used = std::strlen(text);
        std::snprintf(text + used, sizeof(text) - used, " model%u=%s(%u bones)", i,
            body->models[i]->name, static_cast<unsigned>(body->models[i]->numBones));
    }
    bodyDomain.failure = text; return false;
}
bool compositionFor(const clientState_s* info, Composition& output) {
    output = {};
    if (!info || static_cast<unsigned>(info->modelindex) >= 512 || !bodyDomain.modelPointers[info->modelindex]) return false;
    output.count = 1; output.models[0] = bodyDomain.modelPointers[info->modelindex];
    for (unsigned i = 0; i < 6; ++i) {
        if (static_cast<unsigned>(info->attachModelIndex[i]) >= 512 ||
            static_cast<unsigned>(info->attachTagIndex[i]) >= CS_TAGS_COUNT) return false;
        // BG_UpdatePlayerDObj terminates on the configstring name, not on its
        // numeric index. In particular a named index zero is not an empty slot.
        if (!bodyDomain.modelNamed[info->attachModelIndex[i]]) break;
        auto* model = bodyDomain.modelPointers[info->attachModelIndex[i]];
        if (!model) return false;
        output.models[output.count] = model;
        output.tags[output.count++] = bodyDomain.tagIds[info->attachTagIndex[i]];
    }
    return true;
}
bool equal(const Composition& a, const Composition& b) {
    return a.count == b.count && !std::memcmp(a.models, b.models, a.count * sizeof(a.models[0])) &&
        !std::memcmp(a.tags, b.tags, a.count * sizeof(a.tags[0]));
}
const Body* find(const Composition& composition) {
    for (unsigned i = 0; i < bodyDomain.count; ++i)
        if (equal(bodyDomain.bodies[i].composition, composition)) return &bodyDomain.bodies[i];
    return nullptr;
}
bool partSet(const int* bits, unsigned index) {
    return (static_cast<unsigned>(bits[index >> 5]) & (0x80000000u >> (index & 31))) != 0;
}
// Read the actual native duplicate/parent relation, rather than inferring a
// dependency from matching bone names. A duplicate is an alias, not an ordinary
// parent transform, and must retain that distinction in the reduced object.
bool boneRelation(const DObj_s& object, unsigned bone, unsigned& kind, int& parent) {
    const auto* duplicate = reinterpret_cast<const unsigned char*>(SL_ConvertToString(object.duplicateParts));
    unsigned word = 0; std::memcpy(&word, duplicate + (bone >> 5) * sizeof(word), sizeof(word));
    if (word & (0x80000000u >> (bone & 31))) {
        for (unsigned p = 16; p + 1 < object.duplicatePartsSize && duplicate[p]; p += 2) {
            if (duplicate[p] - 1 == bone) { kind = 1; parent = duplicate[p + 1] - 1; return true; }
        }
        return fail("native_duplicate_bone_link_is_missing");
    }
    unsigned base = 0;
    for (unsigned m = 0; m < object.numModels; ++m) {
        const auto* asset = object.models[m];
        if (bone < base + asset->numBones) {
            kind = 0;
            const auto local = bone - base;
            if (local >= asset->numRootBones) parent = static_cast<int>(bone - asset->parentList[local - asset->numRootBones]);
            else {
                const auto value = reinterpret_cast<const unsigned char*>(&object.models[object.numModels])[m];
                parent = value == NO_BONEINDEX ? -1 : value;
            }
            return true;
        }
        base += asset->numBones;
    }
    return fail("native_dependency_bone_is_outside_object");
}
unsigned insertedBone(unsigned bone, unsigned bodyBones, unsigned weaponBones) {
    return bone < bodyBones ? bone : bone + weaponBones;
}
unsigned boneNameId(const DObj_s& object, unsigned bone) {
    // DObjGetBoneName performs this same model-order walk, then converts the
    // actual XModel boneNames entry to text. That entry is already the native
    // interned name ID; converting it back through SL_FindString adds a shared
    // string-table lookup without changing which name DObj/XAnim bind.
    unsigned base = 0;
    for (unsigned m = 0; m < object.numModels; ++m) {
        const auto* model = object.models[m];
        if (bone < base + model->numBones)
            return model->boneNames[bone - base];
        base += model->numBones;
    }
    return 0;
}
bool sameGameplayDependencies(const DObj_s& body, const DObj_s& full,
    const unsigned* critical, unsigned count, unsigned weaponBones) {
    const auto baseBones = body.models[0]->numBones;
    for (unsigned t = 0; t < count; ++t) {
        if (!critical[t]) continue;
        unsigned char reducedBone = UnknownBone, nativeBone = UnknownBone;
        const bool reducedFound = DObjGetBoneIndex(&body, critical[t], &reducedBone) != 0;
        const bool nativeFound = DObjGetBoneIndex(&full, critical[t], &nativeBone) != 0;
        if (reducedFound != nativeFound || (reducedFound && nativeBone != insertedBone(reducedBone, baseBones, weaponBones)))
            return failDetail("weapon_changes_native_gameplay_tag_binding", &full, critical[t]);
        if (!reducedFound) continue;
        int reducedBits[4]{}, nativeBits[4]{};
        DObjGetHierarchyBits(&body, reducedBone, reducedBits); DObjGetHierarchyBits(&full, nativeBone, nativeBits);
        for (unsigned b = 0; b < full.numBones; ++b) {
            const bool inserted = b >= baseBones && b < baseBones + weaponBones;
            const auto reduced = b < baseBones ? b : b - weaponBones;
            const bool wanted = !inserted && partSet(reducedBits, reduced);
            if (partSet(nativeBits, b) != wanted)
                return failDetail("weapon_changes_native_gameplay_bone_hierarchy", &full, critical[t]);
            if (!wanted) continue;
            unsigned reducedKind = 0, nativeKind = 0; int reducedParent = -1, nativeParent = -1;
            if (!boneRelation(body, reduced, reducedKind, reducedParent) || !boneRelation(full, b, nativeKind, nativeParent)) return false;
            const auto wantedParent = reducedParent < 0 ? -1 : static_cast<int>(insertedBone(reducedParent, baseBones, weaponBones));
            if (nativeKind != reducedKind || nativeParent != wantedParent)
                return failDetail("weapon_changes_native_gameplay_bone_parent", &full, critical[t]);
            // Native XAnim maps names in model order too. A retained attachment
            // dependency must receive the same animation binding after insertion.
            const auto tag = boneNameId(body, reduced);
            unsigned char reducedBinding = UnknownBone, nativeBinding = UnknownBone;
            if (!tag || !DObjGetBoneIndex(&body, tag, &reducedBinding) || !DObjGetBoneIndex(&full, tag, &nativeBinding) ||
                nativeBinding != insertedBone(reducedBinding, baseBones, weaponBones))
                return failDetail("weapon_changes_native_gameplay_animation_binding", &full, tag);
        }
    }
    return true;
}
bool weaponsDoNotAffectTags(const Composition& composition, const DObj_s& body) {
    const unsigned critical[] = {scr_const.aim_bone, scr_const.aim_highest_bone, scr_const.aim_vis_bone,
        *controller_names[0], *controller_names[1], *controller_names[2],
        *controller_names[3], *controller_names[4], *controller_names[5]};
    bool baseOnly = true;
    unsigned absent[6]{}; unsigned absentCount = 0;
    for (unsigned t = 0; t < ARRAY_COUNT(critical); ++t) {
        unsigned char bone = UnknownBone;
        const bool found = critical[t] && DObjGetBoneIndex(&body, critical[t], &bone);
        // Stock controller setup permits missing controller tags; aim does not.
        if (!found) {
            if (t < 3) return failDetail("body_missing_native_aim_tag", &body, critical[t]);
            if (critical[t]) absent[absentCount++] = critical[t];
            continue;
        }
        // Exercise the actual native cache guard while preparing assets. A
        // known-missing sentinel must not be confused with an unknown lookup.
        unsigned char missing = 255;
        if (DObjGetBoneIndex(&body, critical[t], &missing)) return fail("native_bone_lookup_cache_guard_changed");
        int bits[4]{}; DObjGetHierarchyBits(&body, bone, bits);
        if (bone >= body.models[0]->numBones) baseOnly = false;
        for (unsigned b = body.models[0]->numBones; b < body.numBones; ++b)
            if (partSet(bits, b)) baseOnly = false;
    }
    // The base model is first in both native objects. Native DObj lookup and
    // XAnim binding select the first model; native duplicate/attachment parents
    // only reference earlier models. Therefore later weapon models cannot
    // change a base-only dependency, even when they share tag_origin. Missing
    // optional controller tags still must not become supplied by a weapon.
    for (unsigned w = 1; w < BG_GetNumWeapons(); ++w) {
        const auto* weapon = BG_GetWeaponDef(w);
        for (unsigned m = 0; m < 17; ++m) {
            const auto* model = m < 16 ? weapon->worldModel[m] : weapon->worldKnifeModel;
            if (!model) continue;
            for (unsigned n = 0; n < absentCount; ++n) {
                unsigned char bone = UnknownBone;
                if (XModelGetBoneIndex(model, absent[n], 0, &bone)) baseOnly = false;
            }
        }
    }
    if (baseOnly) return true;

    // Attachment dependencies need an actual native full-object comparison:
    // a weapon inserted before a head can alter duplicate melding or parent
    // selection. Evaluate native CG_AttachWeapon combinations outside ticks.
    // Duplicate world-model variants are skipped, retaining bounded work.
    clientInfo_t info{};
    for (unsigned w = 1; w < BG_GetNumWeapons(); ++w) {
        const auto* weapon = BG_GetWeaponDef(w); info.iDObjWeapon = w;
        for (unsigned variant = 0; variant < 17; ++variant) {
            const bool knifeOnly = variant == 16;
            if (knifeOnly ? !weapon->worldKnifeModel : !weapon->worldModel[variant]) continue;
            if (!knifeOnly) {
                bool duplicate = false;
                for (unsigned old = 0; old < variant; ++old)
                    if (weapon->worldModel[old] == weapon->worldModel[variant]) { duplicate = true; break; }
                if (duplicate) continue;
            }
            info.weaponModel = knifeOnly ? 0 : static_cast<unsigned char>(variant);
            info.hideWeapon = knifeOnly;
            for (unsigned hand = 0; hand < (knifeOnly ? 1u : 2u); ++hand) {
                info.leftHandGun = hand;
                for (unsigned knife = 0; knife < (!knifeOnly && weapon->worldKnifeModel ? 2u : 1u); ++knife) {
                    info.usingKnife = knifeOnly || knife != 0;
                    DObjModel_s descriptors[ModelCapacity + 2]{}; descriptors[0].model = composition.models[0];
                    unsigned count = CG_AttachWeapon(descriptors, 1, &info), weaponBones = 0;
                    for (unsigned i = 1; i < count; ++i) weaponBones += descriptors[i].model->numBones;
                    // Native itself rejects this impossible composition before
                    // creating any object; it cannot be a supported live case.
                    if (body.numBones + weaponBones > DOBJ_MAX_PARTS) continue;
                    for (unsigned i = 1; i < composition.count; ++i) {
                        descriptors[count].model = composition.models[i]; descriptors[count++].boneName = composition.tags[i];
                    }
                    DObj_s full{}; DObjCreate(descriptors, count, nullptr, &full, 0);
                    const bool equivalent = sameGameplayDependencies(body, full, critical, ARRAY_COUNT(critical), weaponBones);
                    DObjFree(&full);
                    if (!equivalent) return false;
                }
            }
        }
    }
    return true;
}
bool prepare(const Composition& composition) {
    if (find(composition)) return true;
    if (bodyDomain.count == BodyCapacity) return fail("body_composition_capacity_exhausted");
    unsigned bones = 0;
    for (unsigned i = 0; i < composition.count; ++i) {
        bones += composition.models[i]->numBones;
        if (bones > 128) return fail("body_composition_exceeds_native_bone_limit");
        if (!i || !composition.tags[i]) continue;
        bool parentFound = false;
        for (unsigned p = 0; p < i; ++p) {
            unsigned char bone = UnknownBone;
            if (XModelGetBoneIndex(composition.models[p], composition.tags[i], 0, &bone)) { parentFound = true; break; }
        }
        if (!parentFound) return fail("client_attachment_parent_requires_excluded_model");
    }
    auto& body = bodyDomain.bodies[bodyDomain.count];
    body.assetTree = {}; body.assetTree.anims = bodyDomain.animations;
    DObjModel_s descriptors[ModelCapacity]{};
    for (unsigned i = 0; i < composition.count; ++i) {
        descriptors[i].model = composition.models[i]; descriptors[i].boneName = composition.tags[i];
    }
    DObj_s original{};
    DObjCreate(descriptors, composition.count, &body.assetTree, &original, 0);
    if (!weaponsDoNotAffectTags(composition, original)) { DObjFree(&original); return false; }
    body.composition = composition; body.object = original;
    const auto bytes = composition.count * (sizeof(void*) + 1);
    std::memcpy(body.models, original.models, bytes);
    body.object.models = reinterpret_cast<XModel**>(body.models);
    body.object.skel = {}; body.object.locked = 0;
    if (body.object.duplicateParts) SL_AddRefToString(body.object.duplicateParts);
    DObjFree(&original);
    body.validatedWeaponCount = BG_GetNumWeapons();
    ++bodyDomain.count;
    return true;
}

XModel* namedModel(const char* name) {
    for (const auto* model : bodyDomain.modelPointers)
        if (model && !std::strcmp(model->name, name)) return const_cast<XModel*>(model);
    return nullptr;
}
// Prewarm complete, literal character scripts already resident in the loaded
// map. This does not load extra assets, execute scripts, or guess head/body
// combinations. More complex/custom scripts are prepared from accepted native
// metadata by the same asset barrier.
void __cdecl prewarmCharacter(XAssetHeader header, void*) {
    const auto* raw = header.rawfile;
    if (!raw || !raw->name || !raw->buffer || raw->len <= 0 || raw->len > 65536 ||
        std::strncmp(raw->name, "character/", 10) || !std::strstr(raw->name, ".gsc")) return;
    ++bodyDomain.stockScripts;
    Composition composition{};
    unsigned bodyCalls = 0, attachmentCalls = 0;
    bool complete = true;
    const auto* cursor = raw->buffer;
    Com_BeginParseSession(raw->name); Com_SetSpaceDelimited(0); Com_SetKeepStringQuotes(1);
    while (cursor && *cursor) {
        const auto* token = Com_Parse(&cursor)->token;
        const bool setModel = !I_stricmp(token, "setmodel"), attach = !I_stricmp(token, "attach");
        if (!setModel && !attach) continue;
        if (setModel) ++bodyCalls; else ++attachmentCalls;
        if (std::strcmp(Com_Parse(&cursor)->token, "(")) { complete = false; continue; }
        char modelName[256]; I_strncpyz(modelName, Com_Parse(&cursor)->token, sizeof(modelName));
        const auto length = std::strlen(modelName);
        if (length < 2 || modelName[0] != '"' || modelName[length - 1] != '"') { complete = false; continue; }
        modelName[length - 1] = 0; auto* model = namedModel(modelName + 1);
        if (!model) { complete = false; continue; }
        if (setModel) { composition.models[0] = model; continue; }
        if (composition.count >= ModelCapacity - 1 || std::strcmp(Com_Parse(&cursor)->token, ",")) { complete = false; continue; }
        char tagName[128]; I_strncpyz(tagName, Com_Parse(&cursor)->token, sizeof(tagName));
        const auto tagLength = std::strlen(tagName);
        if (tagLength < 2 || tagName[0] != '"' || tagName[tagLength - 1] != '"') { complete = false; continue; }
        tagName[tagLength - 1] = 0;
        const auto tag = tagName[1] ? SL_FindString(tagName + 1) : 0;
        if (tagName[1] && !tag) { complete = false; continue; }
        composition.models[1 + composition.count] = model;
        composition.tags[1 + composition.count++] = static_cast<unsigned short>(tag);
    }
    Com_EndParseSession();
    // Only a single literal base assignment and a single literal attachment
    // sequence is unambiguous without interpreting GSC branches. Reserve half
    // of the fixed cache for accepted future custom compositions.
    if (!complete || bodyCalls != 1 || attachmentCalls > 1 || !composition.models[0] ||
        bodyDomain.count >= BodyCapacity / 2) return;
    ++composition.count;
    const auto before = bodyDomain.count;
    bodyDomain.diagnosticClient = -1; bodyDomain.diagnosticModel = -1;
    if (prepare(composition)) bodyDomain.stockCompositions += bodyDomain.count - before;
    // Optional prewarm candidates cannot reject a match. An accepted instance
    // still performs the complete native guard and reports its concrete model.
    bodyDomain.failure = "";
}
}

bool KHZ_ClientBodyRootTags(const DObj_s* object) {
    if (!object || !object->numModels || !object->models) return false;
    const unsigned aim[] = {scr_const.aim_bone, scr_const.aim_highest_bone, scr_const.aim_vis_bone};
    for (const auto tag : aim) { unsigned char bone = UnknownBone; if (!tag || !DObjGetBoneIndex(object, tag, &bone)) return false; }
    return true;
}
bool KHZ_ClientBodyRefresh(const cgs_t* gameStatic, const snapshot_s* accepted) {
    iassert(Sys_IsMainThread());
    if (!bodyDomain.prepared || !gameStatic || !accepted) return fail("body_refresh_without_prepared_domain");
    std::memcpy(bodyDomain.modelPointers, gameStatic->gameModels, sizeof(bodyDomain.modelPointers));
    for (unsigned i = 0; i < ARRAY_COUNT(bodyDomain.modelNamed); ++i)
        bodyDomain.modelNamed[i] = CL_GetConfigString(0, CS_MODELS + i)[0] != 0;
    for (unsigned i = 0; i < CS_TAGS_COUNT; ++i) {
        const auto* name = CL_GetConfigString(0, CS_TAGS + i);
        bodyDomain.tagIds[i] = name[0] ? static_cast<unsigned short>(SL_FindString(name)) : 0;
        if (name[0] && !bodyDomain.tagIds[i]) return fail("client_attachment_tag_is_not_pinned_by_loaded_assets");
    }
    for (int i = 0; i < accepted->numClients; ++i) {
        const auto& client = accepted->clients[i];
        bool alive = client.clientIndex == accepted->ps.clientNum && (accepted->ps.otherFlags & 6) && accepted->ps.pm_type < PM_DEAD;
        for (int e = 0; !alive && e < accepted->numEntities; ++e)
            alive = accepted->entities[e].eType == ET_PLAYER && accepted->entities[e].number == client.clientIndex &&
                !(accepted->entities[e].lerp.eFlags & EF_DEAD);
        if (!alive) continue;
        bodyDomain.diagnosticClient = client.clientIndex; bodyDomain.diagnosticModel = client.modelindex;
        Composition composition{};
        if (!compositionFor(&client, composition)) return fail("accepted_client_model_composition_is_not_registered");
        if (!prepare(composition)) return false;
    }
    const auto weaponCount = BG_GetNumWeapons();
    for (unsigned i = 0; i < bodyDomain.count; ++i) {
        auto& body = bodyDomain.bodies[i];
        // prepare() has already validated new bodies against this same loaded
        // weapon table. Existing bodies still need the full native comparison
        // whenever an accepted configstring grows the registered weapon set.
        if (body.validatedWeaponCount == weaponCount) continue;
        if (!weaponsDoNotAffectTags(body.composition, body.object)) return false;
        body.validatedWeaponCount = weaponCount;
    }
    bodyDomain.failure = "";
    return true;
}
bool KHZ_ClientBodyPrepare(const cgs_t* gameStatic, XAnim_s* animations, const snapshot_s* accepted) {
    iassert(Sys_IsMainThread());
    if (bodyDomain.prepared || !gameStatic || !animations || !accepted) return fail("invalid_body_prepare_arguments");
    bodyDomain.animations = animations; bodyDomain.prepared = true;
    if (!KHZ_ClientBodyRefresh(gameStatic, accepted)) { KHZ_ClientBodyRelease(); return false; }
    DB_EnumXAssets(ASSET_TYPE_RAWFILE, prewarmCharacter, nullptr, false);
    return true;
}
void KHZ_ClientBodyRelease() {
    iassert(Sys_IsMainThread());
    for (unsigned i = 0; i < bodyDomain.count; ++i) {
        auto& body = bodyDomain.bodies[i];
        if (body.object.duplicateParts) SL_RemoveRefToStringOfSize(body.object.duplicateParts, body.object.duplicatePartsSize);
        body.object = {}; body.assetTree = {}; body.composition = {}; body.validatedWeaponCount = 0;
    }
    bodyDomain.count = 0; bodyDomain.prepared = false; bodyDomain.animations = nullptr;
    bodyDomain.stockScripts = 0; bodyDomain.stockCompositions = 0;
    for (auto& pointer : bodyDomain.modelPointers) pointer = nullptr;
}
const DObj_s* KHZ_ClientBodyForClient(const clientState_s* client) {
    Composition composition{};
    if (!bodyDomain.prepared || !compositionFor(client, composition)) return nullptr;
    const auto* body = find(composition); return body ? &body->object : nullptr;
}
unsigned KHZ_ClientBodyCount() { return bodyDomain.prepared ? bodyDomain.count : 0; }
const DObj_s* KHZ_ClientBodyAt(unsigned index) { return bodyDomain.prepared && index < bodyDomain.count ? &bodyDomain.bodies[index].object : nullptr; }
const char* KHZ_ClientBodyFailureReason() { return bodyDomain.failure ? bodyDomain.failure : ""; }
unsigned KHZ_ClientBodyStockScripts() { return bodyDomain.stockScripts; }
unsigned KHZ_ClientBodyStockCompositions() { return bodyDomain.stockCompositions; }
