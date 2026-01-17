/**
 * @file App_Audio.h
 * @brief 音频控制头文件 (修复版)
 */
#ifndef APP_AUDIO_H
#define APP_AUDIO_H

#include <Arduino.h>
#include <WiFi.h>
#include "freertos/ringbuf.h" 

// [核心] 统一采样率 16000 (配合 ADPCM)
#define AUDIO_SAMPLE_RATE  16000 
// [核心] 播放缓冲大小
#define PLAY_BUFFER_SIZE   (1024 * 200)

class AppAudio {
public: // === 这里的变量和函数都是公开的 ===

    // RingBuffer 句柄 (必须公开，否则后台任务无法访问)
    RingbufHandle_t playRingBuf = NULL;
    
    // 初始化
    void init();

    // 播放/录音控制
    void setVolume(uint8_t vol);
    void setMicGain(uint8_t gain);
    void playToneAsync(int freq, int duration_ms);
    void startRecording();
    void stopRecording();

    // [修复] 确保这两个函数在 public 里
    void playStream(Client* client, int length);
    void playChunk(uint8_t* data, size_t len);
    void pushToPlayBuffer(uint8_t* data, size_t len);

    // 录音内部任务
    void _recordTask(void *param);

    // 录音缓冲
    uint8_t *record_buffer = NULL;       
    uint32_t record_data_len = 0;        
    const uint32_t MAX_RECORD_SIZE = 1024 * 512; 

private: // === 私有变量 ===
    void createWavHeader(uint8_t *header, uint32_t totalDataLen, uint32_t sampleRate, uint8_t sampleBits, uint8_t numChannels);

    TaskHandle_t recordTaskHandle = NULL;
    volatile bool isRecording = false;
    TaskHandle_t playTaskHandle = NULL;
};

extern AppAudio MyAudio;

#ifdef __cplusplus
extern "C" {
#endif
void Audio_Play_Click();
void Audio_Record_Start();
void Audio_Record_Stop();
#ifdef __cplusplus
}
#endif

#endif