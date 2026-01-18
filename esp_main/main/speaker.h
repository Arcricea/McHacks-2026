#ifndef SPEAKER_H
#define SPEAKER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and run speaker test/playback
 * This function initializes the SD card, lists files, and plays the configured WAV file
 */
void speaker_main(void);

/**
 * @brief Set the playback speed multiplier
 * 
 * @param speed Playback speed multiplier (1.0 = normal, 2.0 = 2x speed, 0.5 = half speed)
 *              Valid range: 0.25x to 4.0x (values outside this range will be clamped)
 * 
 * @note This affects the pitch of the audio (faster = higher pitch, slower = lower pitch)
 * @note Speed must be set BEFORE calling play functions
 * @note The final sample rate is clamped to hardware limits (8000-48000 Hz for PDM)
 *       For a 44100 Hz WAV file: max speed ~1.08x, min speed ~0.18x
 */
void set_playback_speed(float speed);

/**
 * @brief Get the current playback speed multiplier
 *
 * @return Current playback speed (1.0 = normal speed)
 */
float get_playback_speed(void);

#ifdef __cplusplus
}
#endif

#endif // SPEAKER_H
