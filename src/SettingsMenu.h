// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Settings.h"
namespace Aottg::SettingsMenu {
    void Register(const Settings&);
    bool Blocking();
    int ReservedKey();
    void PublishStatus(bool active,float activeScale);
    void PublishSkill(size_t index);
}
