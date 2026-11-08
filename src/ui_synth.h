#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

namespace fgvk {
// Synthetic premultiplied UI layer for XeSS-FG UI modes 3/5: computes B-(1-a)*C from the
// presented frame and the captured HUD-less frame and stores it into a shared (VK/DX) target.
void SynthUiInit(VkDevice, VkPhysicalDevice, PFN_vkGetDeviceProcAddr, VkFormat, VkExtent2D);
void SynthUiDestroy();
bool SynthUiAvailable();
// Records barriers + compute + format-compatible copy into target (queue-family handoff EXTERNAL<->family, mirroring the transport image).
bool SynthUiRecord(VkCommandBuffer cmd, VkImage presented, VkImage hudless, VkImage target, uint32_t graphicsFamily, bool targetFirstUse);
}
