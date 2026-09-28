/* Project-private observation ABI v1. Not an SDL API or dynapi ordinal. */
#ifndef TOOIE_SDL_OBSERVER_H
#define TOOIE_SDL_OBSERVER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(TOOIE_SDL_OBSERVER_BUILD)
#define TOOIE_SDL_EXPORT __declspec(dllexport)
#else
#define TOOIE_SDL_EXPORT
#endif
enum { TOOIE_SDL_OK=0, TOOIE_SDL_ABI=-1, TOOIE_SDL_CAPACITY=-2,
       TOOIE_SDL_STATE=-3, TOOIE_SDL_OPEN_DEVICES=-4, TOOIE_SDL_ALLOC=-5,
       TOOIE_SDL_WRITERS=-6, TOOIE_SDL_ARGUMENT=-7 };
enum { TOOIE_SDL_SOURCE_DRAIN=1, TOOIE_SDL_GET_BUFFER=2,
       TOOIE_SDL_RELEASE_BUFFER=3, TOOIE_SDL_WAIT=4, TOOIE_SDL_PADDING=5,
       TOOIE_SDL_STREAM_PUT=6, TOOIE_SDL_STREAM_AVAILABLE=7,
       TOOIE_SDL_STREAM_GET=8, TOOIE_SDL_OUTPUT_SILENCE=9,
       TOOIE_SDL_DEVICE_SPEC=10, TOOIE_SDL_MIX_FORMAT=11,
       TOOIE_SDL_WASAPI_SPEC=12 };
typedef struct TooieSDL_Record {
    uint64_t ordinal, generation, begin_qpc, end_qpc;
    uint32_t kind, device_id, thread_id, flags;
    uint64_t values[10];
} TooieSDL_Record;
typedef struct TooieSDL_Snapshot {
    uint32_t version, record_size;
    uint64_t count, capacity, attempted, overflow, qpc_frequency, open_devices;
} TooieSDL_Snapshot;
/* Controls require external serialization. Begin precedes device open;
   Snapshot/Release follow all device closes and caller joins. Returned rows
   remain owned by DLL and immutable until Release. Never unload first. */
TOOIE_SDL_EXPORT int TooieSDL_AudioObserveBegin(uint32_t version, uint32_t record_size, uint64_t capacity);
TOOIE_SDL_EXPORT int TooieSDL_AudioObserveSnapshotAfterClose(uint32_t version, uint32_t summary_size,
    TooieSDL_Snapshot *summary, const TooieSDL_Record **records);
TOOIE_SDL_EXPORT int TooieSDL_AudioObserveRelease(void);
/* Internal producers: no allocation, I/O, locks or additional SDL queries. */
uint64_t TooieSDL_ObserveTick(void);
uint64_t TooieSDL_ObserveDeviceOpen(void);
void TooieSDL_ObserveDeviceClosed(uint64_t generation);
void TooieSDL_ObserveEmit(uint32_t kind, uint64_t generation, uint32_t device_id, uint64_t begin,
    uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
    uint64_t f, uint64_t g, uint64_t h, uint64_t i, uint64_t j);
#ifdef __cplusplus
}
#endif
#endif
