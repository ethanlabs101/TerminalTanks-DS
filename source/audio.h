/* Music and sound effects: the PC game's soundtrack and effects, rendered by tools/gen_audio.py and played here.
 * Music streams from nitro:/audio/music.pak (IMA-ADPCM, decoded in the VBlank interrupt into a looping PCM ring on
 * sound channel 0); effects come from sfx.pak through a small RAM cache and play on hardware ADPCM voices. */
#ifndef TT_AUDIO_H
#define TT_AUDIO_H
#include <stdbool.h>
#include <stdint.h>
#include "gen/audio_data.h"

enum { MOOD_MENU, MOOD_STORY, MOOD_RESULT };
enum { AUDIO_OFF = 0, AUDIO_BEEP = 1, AUDIO_ON = 2 };

bool audio_init(void);                 /* after storage is up; false if the packs are missing (then it stays silent) */
void audio_update(float dt);           /* once per frame: disk reads, fades, ducking */
void audio_irq(void);                  /* from the VBlank interrupt: keeps the music ring filled */
void audio_settings(int mode, int music_vol, int sfx_vol);   /* volumes 0..10 */
void audio_music(int song, float fade);/* -1 = silence; the same song again keeps playing */
void audio_mood(int mood);
void audio_sfx(int id, float vol, float pan, float rate);     /* pan -1..1 */
void audio_sfx_ammo(bool hit, int ammo, float vol, float pan);
int audio_current_song(void);
void audio_loop(bool on);              /* false: the current song plays once, then audio_take_ended() reports it */
bool audio_take_ended(void);
void audio_position(int *pos_s, int *dur_s);
void audio_spectrum(uint8_t out[16]);   /* 0..8 per band, for the Jukebox analyser */
bool audio_ready(void);
#endif
