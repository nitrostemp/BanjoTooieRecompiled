// Pinned SDL's Windows consumer headers leave DECLSPEC empty. Taking a normal
// function address can therefore identify an executable import thunk. This
// isolated TU declares the same linked SDL import as dllimport so the returned
// pointer is read from that import's IAT, never a second module/name lookup.
#define DECLSPEC __declspec(dllimport)
#define SDL_MAIN_HANDLED
#include "SDL.h"
#include "sdl_audio_observation.hpp"
namespace tooie::sdl_observation {
const void* imported_anchor() noexcept {
    return reinterpret_cast<const void*>(&SDL_QueueAudio);
}
}
