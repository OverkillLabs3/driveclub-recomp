// SPDX-License-Identifier: GPL-2.0-or-later
#include "vehicle_motion_policy.h"
#include "motion_history.h"
#include <cassert>
#include <cstdio>
int main() {
    using namespace Vulkan::VehicleMotionScope;
    assert(!ExplicitAll(nullptr)&&!ExplicitAll("0")&&ExplicitAll("1"));
    for(bool vehicle:{false,true}) for(bool override_all:{false,true})
        for(unsigned count=0;count<=8;++count) {
            const bool expected=count>=(vehicle||override_all?3u:5u);
            assert(Admission(count,vehicle,override_all,true)==expected);
            assert(!Admission(count,vehicle,override_all,false));
        }
    assert(!Admission(3,false,ExplicitAll(nullptr),true));
    assert(!Admission(3,false,ExplicitAll("0"),true));
    assert(Admission(3,true,ExplicitAll(nullptr),true));
    assert(Admission(3,true,ExplicitAll("0"),true));
    assert(Broad(true,false)&&!Broad(false,false));
    assert(HashModelState(true,416)&&!HashModelState(false,416));
    // The actual legacy size heuristic can mistake a large world constant
    // buffer for a skeleton. Count admission must reject that three-RT draw.
    assert(Vulkan::Motion::ClassifyBuffer(672)==Vulkan::Motion::BufferRole::Skeleton);
    assert(!Admission(3,false,false,true));
    // Rigid car model buffers are Other in that heuristic; positive vehicle
    // provenance must retain broad instrumentation without a palette.
    assert(Vulkan::Motion::ClassifyBuffer(416)==Vulkan::Motion::BufferRole::Other);
    assert(Admission(3,true,false,true)&&Broad(true,false));
    assert(VehicleProgram(0xc6c425e2)&&!VehicleProgram(0x6c4ce233));
    assert(!VehicleProgram(0)&&!VehicleProgram(0x8d9dd0a2));
    std::puts("PASS vehicle3RT broad; nonvehicle3RT default/ALL0 denied; legacy5RT eligible; guards retained; nonvehicle416 hash excluded");
}
