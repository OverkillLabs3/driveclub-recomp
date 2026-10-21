// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Changed for this port. The upstream sources of the vendored tree are listed in
// gpu/VENDOR.txt and in NOTICE.
//
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/np/np_error.h"
#include <atomic>
#include "core/libraries/np/np_party.h"

namespace Libraries::Np::NpParty {

// The game polls these stubs all the time: only the first few calls are logged.
static void NpPartyStubLog() {
    static std::atomic<u32> calls{0};
    if (calls.fetch_add(1) < 3) {
        LOG_ERROR(Lib_NpParty, "(STUBBED) called (later calls are not logged)");
    }
}

s32 PS4_SYSV_ABI sceNpPartyCheckCallback() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyCreate() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyCreateA() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyGetId() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyGetMemberInfo() {
    NpPartyStubLog();
    return ORBIS_NP_PARTY_ERROR_NOT_IN_PARTY;
}

s32 PS4_SYSV_ABI sceNpPartyGetMemberInfoA() {
    NpPartyStubLog();
    return ORBIS_NP_PARTY_ERROR_NOT_IN_PARTY;
}

s32 PS4_SYSV_ABI sceNpPartyGetMembers() {
    NpPartyStubLog();
    return ORBIS_NP_PARTY_ERROR_NOT_IN_PARTY;
}

s32 PS4_SYSV_ABI sceNpPartyGetMembersA() {
    NpPartyStubLog();
    return ORBIS_NP_PARTY_ERROR_NOT_IN_PARTY;
}

s32 PS4_SYSV_ABI sceNpPartyGetMemberSessionInfo() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyGetMemberVoiceInfo() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyGetState(OrbisNpPartyState* state) {
    NpPartyStubLog();
    *state = OrbisNpPartyState::NotInParty;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyGetStateAsUser() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyGetStateAsUserA() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyGetVoiceChatPriority() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyInitialize() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyJoin() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyLeave() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyRegisterHandler() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyRegisterHandlerA() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyRegisterPrivateHandler() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartySendBinaryMessage() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartySetVoiceChatPriority() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyShowInvitationList() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyShowInvitationListA() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyTerminate() {
    NpPartyStubLog();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpPartyUnregisterPrivateHandler() {
    NpPartyStubLog();
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("3e4k2mzLkmc", "libSceNpParty", 1, "libSceNpParty", sceNpPartyCheckCallback);
    LIB_FUNCTION("nOZRy-slBoA", "libSceNpParty", 1, "libSceNpParty", sceNpPartyCreate);
    LIB_FUNCTION("XQSUbbnpPBA", "libSceNpParty", 1, "libSceNpParty", sceNpPartyCreateA);
    LIB_FUNCTION("DRA3ay-1DFQ", "libSceNpParty", 1, "libSceNpParty", sceNpPartyGetId);
    LIB_FUNCTION("F1P+-wpxQow", "libSceNpParty", 1, "libSceNpParty", sceNpPartyGetMemberInfo);
    LIB_FUNCTION("v2RYVGrJDkM", "libSceNpParty", 1, "libSceNpParty", sceNpPartyGetMemberInfoA);
    LIB_FUNCTION("T2UOKf00ZN0", "libSceNpParty", 1, "libSceNpParty", sceNpPartyGetMembers);
    LIB_FUNCTION("TaNw7W25QJw", "libSceNpParty", 1, "libSceNpParty", sceNpPartyGetMembersA);
    LIB_FUNCTION("4gOMfNYzllw", "libSceNpParty", 1, "libSceNpParty",
                 sceNpPartyGetMemberSessionInfo);
    LIB_FUNCTION("EKi1jx59SP4", "libSceNpParty", 1, "libSceNpParty", sceNpPartyGetMemberVoiceInfo);
    LIB_FUNCTION("aEzKdJzATZ0", "libSceNpParty", 1, "libSceNpParty", sceNpPartyGetState);
    LIB_FUNCTION("o7grRhiGHYI", "libSceNpParty", 1, "libSceNpParty", sceNpPartyGetStateAsUser);
    LIB_FUNCTION("EjyAI+QNgFw", "libSceNpParty", 1, "libSceNpParty", sceNpPartyGetStateAsUserA);
    LIB_FUNCTION("-lc6XZnQXvM", "libSceNpParty", 1, "libSceNpParty",
                 sceNpPartyGetVoiceChatPriority);
    LIB_FUNCTION("lhYCTQmBkds", "libSceNpParty", 1, "libSceNpParty", sceNpPartyInitialize);
    LIB_FUNCTION("RXNCDw2GDEg", "libSceNpParty", 1, "libSceNpParty", sceNpPartyJoin);
    LIB_FUNCTION("J8jAi-tfJHc", "libSceNpParty", 1, "libSceNpParty", sceNpPartyLeave);
    LIB_FUNCTION("kA88gbv71ao", "libSceNpParty", 1, "libSceNpParty", sceNpPartyRegisterHandler);
    LIB_FUNCTION("+v4fVHMwFWc", "libSceNpParty", 1, "libSceNpParty", sceNpPartyRegisterHandlerA);
    LIB_FUNCTION("zo4G5WWYpKg", "libSceNpParty", 1, "libSceNpParty",
                 sceNpPartyRegisterPrivateHandler);
    LIB_FUNCTION("U6VdUe-PNAY", "libSceNpParty", 1, "libSceNpParty", sceNpPartySendBinaryMessage);
    LIB_FUNCTION("nazKyHygHhY", "libSceNpParty", 1, "libSceNpParty",
                 sceNpPartySetVoiceChatPriority);
    LIB_FUNCTION("-MFiL7hEnPE", "libSceNpParty", 1, "libSceNpParty", sceNpPartyShowInvitationList);
    LIB_FUNCTION("yARHEYLajs0", "libSceNpParty", 1, "libSceNpParty", sceNpPartyShowInvitationListA);
    LIB_FUNCTION("oLYkibiHqRA", "libSceNpParty", 1, "libSceNpParty", sceNpPartyTerminate);
    LIB_FUNCTION("zQ7gIvt11Pc", "libSceNpParty", 1, "libSceNpParty",
                 sceNpPartyUnregisterPrivateHandler);
    LIB_FUNCTION("nOZRy-slBoA", "libSceNpPartyCompat", 1, "libSceNpParty", sceNpPartyCreate);
    LIB_FUNCTION("F1P+-wpxQow", "libSceNpPartyCompat", 1, "libSceNpParty", sceNpPartyGetMemberInfo);
    LIB_FUNCTION("T2UOKf00ZN0", "libSceNpPartyCompat", 1, "libSceNpParty", sceNpPartyGetMembers);
    LIB_FUNCTION("o7grRhiGHYI", "libSceNpPartyCompat", 1, "libSceNpParty",
                 sceNpPartyGetStateAsUser);
    LIB_FUNCTION("kA88gbv71ao", "libSceNpPartyCompat", 1, "libSceNpParty",
                 sceNpPartyRegisterHandler);
    LIB_FUNCTION("-MFiL7hEnPE", "libSceNpPartyCompat", 1, "libSceNpParty",
                 sceNpPartyShowInvitationList);
};

} // namespace Libraries::Np::NpParty