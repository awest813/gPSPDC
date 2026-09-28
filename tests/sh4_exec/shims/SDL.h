/* Test-only SDL shim for the qemu-sh4 dynarec harness.  The production
   headers reference a handful of SDL types and calls; none of them run
   in the harness. */
#ifndef GPSP_SH4_EXEC_SDL_SHIM_H
#define GPSP_SH4_EXEC_SDL_SHIM_H

typedef struct SDL_mutex SDL_mutex;
typedef struct SDL_cond SDL_cond;
typedef struct SDL_Surface SDL_Surface;

void SDL_PauseAudio(int pause_on);
int SDL_LockMutex(SDL_mutex *mutex);
int SDL_UnlockMutex(SDL_mutex *mutex);

#endif
