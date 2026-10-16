// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Settings.h"
#include <SKSE/SKSE.h>
namespace Aottg::Runtime {
    void SelectSkill(size_t index); // Game thread, shared by the menu and cycle hotkey.
    void ApplySettings(const Settings&);
    void SetAutomaticRestore(bool enabled);
    bool Install();
    bool Initialize();
    void Exit(const char* reason,bool automatic=false);
    void OnMessage(SKSE::MessagingInterface::Message* message);
}
