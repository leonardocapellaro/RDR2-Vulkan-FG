#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

namespace fgvk {
// Synthetic premultiplied UI layer for XeSS-FG UI modes 3/5: computes B-(1-a)*C from the
// presented frame and the captured HUD-less frame and stores it into a shared (VK/DX) target.
// With XeSSSynthSmooth (0..100, default 35) the raw layer additionally passes through a
// temporal EMA accumulator (mix(prev, raw, weight)) before reaching the tagged target.
void SynthUiInit(VkDevice, VkPhysicalDevice, PFN_vkGetDeviceProcAddr, VkFormat, VkExtent2D);
void SynthUiDestroy();
bool SynthUiAvailable();
// Drops the temporal accumulator's contents (next frame restarts from raw). Call on provider
// history resets / resolution changes so a new epoch can never inherit another epoch's UI.
void SynthUiReset();
// Records barriers + compute + format-compatible copy into target (queue-family handoff EXTERNAL<->family, mirroring the transport image).
bool SynthUiRecord(VkCommandBuffer cmd, VkImage presented, VkImage hudless, VkImage target, uint32_t graphicsFamily, bool targetFirstUse);
}
