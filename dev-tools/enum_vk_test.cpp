// Context device enumeration test: does the Vulkan device show up and
// can VulkanIntersectionDevice be instantiated? Mirrors enum_metal_test.
#include "luxrays/core/context.h"

// same circular-dep stub the metal tests need (slg debug handler is
// referenced by ocldevice.cpp but lives in the slg library)
namespace slg { void (*SLG_DebugHandler)(const char *msg) = nullptr; }
#include "luxrays/core/device.h"
#include <iostream>
using namespace luxrays;

static void logCB(const char *msg) { std::cout << "[log] " << msg << std::endl; }

int main() {
    Context ctx(logCB);
    auto descs = ctx.GetAvailableDeviceDescriptions();
    std::cout << "devices: " << descs.size() << std::endl;
    for (size_t i = 0; i < descs.size(); ++i) {
        std::cout << "  [" << i << "] " << descs[i].get().GetName()
                  << " type=0x" << std::hex << (unsigned)descs[i].get().GetType()
                  << std::dec
                  << " CUs=" << descs[i].get().GetComputeUnits()
                  << " maxMem=" << (descs[i].get().GetMaxMemory() / (1024*1024)) << "MB"
                  << std::endl;
    }
    int vk = 0;
    for (auto &d : descs) if (d.get().GetType() & DEVICE_TYPE_VULKAN_ALL) ++vk;
    std::cout << (vk > 0 ? "VK_ENUM: PASS" : "VK_ENUM: FAIL") << std::endl;
    if (!vk) return 1;

    // Create the Vulkan intersection device (the engine's entry path)
    DeviceDescriptions vkDescs;
    for (auto &d : descs) if (d.get().GetType() & DEVICE_TYPE_VULKAN_ALL) vkDescs.push_back(d);
    auto idevices = ctx.AddIntersectionDevices(vkDescs);
    std::cout << "created " << idevices.size() << " intersection device(s)" << std::endl;
    const bool ok = (idevices.size() == 1);
    std::cout << (ok ? "VK_CREATE: PASS" : "VK_CREATE: FAIL") << std::endl;
    return ok ? 0 : 1;
}
