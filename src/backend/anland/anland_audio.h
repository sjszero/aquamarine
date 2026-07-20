// src/backend/anland/anland_audio.h
#ifndef ANLAND_AUDIO_H
#define ANLAND_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

int  anland_audio_start(void);
void anland_audio_stop(void);
void anland_audio_set_fd(int audio_fd);

#ifdef __cplusplus
}
#endif

#endif