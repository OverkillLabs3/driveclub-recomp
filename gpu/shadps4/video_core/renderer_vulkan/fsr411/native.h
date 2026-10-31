// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "fsr411.h"

namespace Fsr411 {

class NativeUpscaler {
public:
    NativeUpscaler(VkPhysicalDevice physical, VkDevice device, const Frame& frame);
    ~NativeUpscaler();
    bool Ready() const;
    bool Record(const Frame& frame, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Fsr411
