#pragma once
#include "SmokeBindings.h"
#include "core/game_time.h"

namespace cs2bv::smoke_capture {
struct Runtime
{
    bool (*mainThread)();
    bool (*faulted)();
    void (*fault)(const char*);
    game_time::Sample (*readClock)();
};

// Bindings and callbacks outlive capture hooks.
void Initialize(const smoke_bindings::Bindings& bindings, Runtime runtime);
bool CreateEventHooks();
bool CreateSourceHooks();
// Enable observers before traversal, then emitters.
bool EnableObservers();
bool EnableEmitters();
void RemoveEventHooks();
void RemoveSourceHooks();
void Clear();
void ResetEffects(bool clearSources);
// Storage is stable. Only capture writes events.
const smoke_effects::HoleCache& Effects();
void EnableBullets(bool enabled);
bool BulletsEnabled();
} // namespace cs2bv::smoke_capture
