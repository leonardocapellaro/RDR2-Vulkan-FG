#pragma once
#include <cstdint>
namespace fgvk {
// This project's Nexus page. No user-configurable update source.
// Fork build: the upstream Nexus mod id pointed at the original author's page.
// A fork must not ping or link someone else's release page, so the id is 0 and
// StartUpdateCheck() exits early (UpdateState::NotConfigured): no network call,
// no update banner, no browser launch.
#ifndef RDR2FG_NEXUS_MOD_ID
#define RDR2FG_NEXUS_MOD_ID 0
#endif
inline constexpr uint32_t kNexusModId=RDR2FG_NEXUS_MOD_ID;
}
