/**
 * @file App_Audio.cpp
 * @brief 包含 RingBuffer 缓冲机制 (修复编译错误版)
 */
#include "App_Audio.h"
#include "Pin_Config.h" 
#include "AudioTools.h"
#include "AudioBoard.h"

AppAudio MyAudio;

static DriverPins my_pins;
static AudioBoard board(AudioDriverES8311, my_pins);
static I2SStream i2s; 

// ==========================================
//  前向声明 (解决 "was not declared" 错误)
// ==========================================
void audioPlayTask(void *param);
void playToneTaskWrapper(void *param);
void recordTaskWrapper(void *param);

struct ToneParams {
    int freq;
    int duration;
};

// ==========================================
//  AppAudio 类实现
// ==========================================

void AppAudio::init() {
    Serial.println("[Audio] Init...");
    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, LOW); 

    my_pins.addI2C(PinFunction::CODEC, PIN_I2C_SCL, PIN_I2C_SDA, 0); 
    my_pins.addI2S(PinFunction::CODEC, PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT, PIN_I2S_DIN);
    my_pins.addPin(PinFunction::PA, PIN_PA_EN, PinLogic::Output);

    CodecConfig cfg;
    cfg.input_device = ADC_INPUT_ALL; 
    cfg.output_device = DAC_OUTPUT_ALL;
    cfg.i2s.bits = BIT_LENGTH_16BITS;
    cfg.i2s.rate = RATE_24K; // 硬件 16k
    cfg.i2s.fmt = I2S_NORMAL; 

    if (board.begin(cfg)) Serial.println("[Audio] Codec OK");
    else Serial.println("[Audio] Codec FAIL");

    board.setVolume(25);      
    board.setInputVolume(0); 

    auto config = i2s.defaultConfig(RXTX_MODE); 
    config.pin_bck = PIN_I2S_BCLK;
    config.pin_ws = PIN_I2S_LRCK;
    config.pin_data = PIN_I2S_DOUT;
    config.pin_data_rx = PIN_I2S_DIN;
    config.pin_mck = PIN_I2S_MCLK; 
    config.sample_rate = AUDIO_SAMPLE_RATE; // 使用宏定义 16000
    config.bits_per_sample = 16;
    config.channels = 2;       
    config.use_apll = true;    
    
    if (i2s.begin(config)) Serial.println("[Audio] I2S OK");
    else Serial.println("[Audio] I2S FAIL");

    // 录音缓冲
    if (psramFound()) record_buffer = (uint8_t *)ps_malloc(MAX_RECORD_SIZE);
    else record_buffer = (uint8_t *)malloc(MAX_RECORD_SIZE);

    // [创建 RingBuffer]
    // 这里的 PLAY_BUFFER_SIZE 来自头文件，现在应该能找到了
    playRingBuf = xRingbufferCreate(PLAY_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (playRingBuf == NULL) {
        Serial.println("[Audio] RingBuffer Create Failed!");
    } else {
        Serial.println("[Audio] RingBuffer Created.");
    }

    // [启动播放任务]
    // 现在 audioPlayTask 已经在顶部声明过了，这里不会报错
    xTaskCreatePinnedToCore(audioPlayTask, "AudioPlay", 4096, this, 5, &playTaskHandle, 1);
    
    playToneAsync(1000, 200);
}

void AppAudio::setVolume(uint8_t vol) { board.setVolume(vol); }
void AppAudio::setMicGain(uint8_t gain) { board.setInputVolume(gain); }

void AppAudio::playToneAsync(int freq, int duration_ms) {
    ToneParams *params = (ToneParams*)malloc(sizeof(ToneParams));
    if(params) {
        params->freq = freq;
        params->duration = duration_ms;
        xTaskCreate(playToneTaskWrapper, "PlayTone", 4096, params, 2, NULL);
    }
}


void AppAudio::pushToPlayBuffer(uint8_t* data, size_t len) {
    if (playRingBuf == NULL || data == NULL || len == 0) return;
    
    // 循环尝试发送，直到成功
    // 如果缓冲区满了，network任务会在这里暂缓，自然就限制了下载速度（流量控制）
    while (xRingbufferSend(playRingBuf, data, len, pdMS_TO_TICKS(10)) != pdTRUE) {
        // 缓冲区满了，给播放任务一点时间去消耗数据
        vTaskDelay(1); 
    }
}

// [核心] 兼容旧接口 - 直接调用 push
void AppAudio::playChunk(uint8_t* data, size_t len) {
    pushToPlayBuffer(data, len);
}

// [核心] 流式播放 - 边下边推
void AppAudio::playStream(Client* client, int length) {
    if (!client || length <= 0) return;
    Serial.printf("[Audio] Stream Push: %d bytes\n", length);
    const int buff_size = 1024; 
    uint8_t buff[buff_size]; 
    int remaining = length;
    while (remaining > 0 && client->connected()) {
        int max_read = (remaining > buff_size) ? buff_size : remaining;
        int bytesIn = 0;
        unsigned long startWait = millis();
        while (bytesIn < max_read && millis() - startWait < 2000) {
             if (client->available()) {
                 int r = client->read(buff + bytesIn, max_read - bytesIn);
                 if (r > 0) bytesIn += r;
             } else delay(1);
        }
        if (bytesIn == 0) break; 
        pushToPlayBuffer(buff, bytesIn);
        remaining -= bytesIn;
    }
}

// ==========================================
//  IMA ADPCM Helper (Tables & Structs)
// ==========================================
const int8_t index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

const int step_size_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3326, 3658, 4024, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

struct AdpcmState {
    int32_t valprev;
    int8_t index;
};

// ==========================================
//  IMA ADPCM Decoder
// ==========================================
int16_t adpcm_decode(uint8_t code, AdpcmState *state) {
    int step = step_size_table[state->index];
    int diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += (step >> 1);
    if (code & 1) diffq += (step >> 2);
    
    if (code & 8) state->valprev -= diffq;
    else state->valprev += diffq;
    
    if (state->valprev > 32767) state->valprev = 32767;
    else if (state->valprev < -32768) state->valprev = -32768;
    
    state->index += index_table[code];
    if (state->index < 0) state->index = 0;
    if (state->index > 88) state->index = 88;
    
    return (int16_t)state->valprev;
}

// ==========================================
//  IMA ADPCM Encoder
// ==========================================
uint8_t adpcm_encode(int16_t sample1, int16_t sample2, AdpcmState *state) {
    uint8_t code = 0;
    
    // Sample 1
    int diff = sample1 - state->valprev;
    int sign = (diff < 0) ? 8 : 0;
    if (diff < 0) diff = -diff;
    
    int step = step_size_table[state->index];
    int delta = 0;
    int vpdiff = (step >> 3);
    
    if (diff >= step) { delta |= 4; diff -= step; vpdiff += step; }
    if (diff >= (step >> 1)) { delta |= 2; diff -= (step >> 1); vpdiff += (step >> 1); }
    if (diff >= (step >> 2)) { delta |= 1; vpdiff += (step >> 2); }
    
    if (sign) state->valprev -= vpdiff;
    else state->valprev += vpdiff;
    
    if (state->valprev > 32767) state->valprev = 32767;
    else if (state->valprev < -32768) state->valprev = -32768;
    
    state->index += index_table[delta | sign];
    if (state->index < 0) state->index = 0;
    if (state->index > 88) state->index = 88;
    
    code = (delta | sign); // Low nibble

    // Sample 2
    diff = sample2 - state->valprev;
    sign = (diff < 0) ? 8 : 0;
    if (diff < 0) diff = -diff;
    
    step = step_size_table[state->index];
    delta = 0;
    vpdiff = (step >> 3);
    
    if (diff >= step) { delta |= 4; diff -= step; vpdiff += step; }
    if (diff >= (step >> 1)) { delta |= 2; diff -= (step >> 1); vpdiff += (step >> 1); }
    if (diff >= (step >> 2)) { delta |= 1; vpdiff += (step >> 2); }
    
    if (sign) state->valprev -= vpdiff;
    else state->valprev += vpdiff;
    
    if (state->valprev > 32767) state->valprev = 32767;
    else if (state->valprev < -32768) state->valprev = -32768;
    
    state->index += index_table[delta | sign];
    if (state->index < 0) state->index = 0;
    if (state->index > 88) state->index = 88;
    
    code |= ((delta | sign) << 4); // High nibble
    
    return code;
}

void AppAudio::startRecording() {
    if (isRecording) return;
    if (!record_buffer) return;
    board.setInputVolume(85); 
    
    // [ADPCM] 重置压缩后长度计数。无需保留 44字节头，因为我们传的是纯数据
    record_data_len = 0; 
    
    isRecording = true;
    xTaskCreate(recordTaskWrapper, "RecTask", 8192, this, 10, &recordTaskHandle);
    Serial.println("[Audio] Start Rec (ADPCM)");
}

void AppAudio::stopRecording() {
    isRecording = false; 
    delay(100); 
    board.setInputVolume(0); 
    // [ADPCM] 这里不再需要生成 WAV 头，因为数据是 ADPCM
    Serial.printf("[Audio] Stop Rec. Encoded Size: %d\n", record_data_len);
}

void AppAudio::_recordTask(void *param) {
    const size_t read_size = 1024; // 读取 PCM 字节 (512 samples)
    uint8_t temp_buf[read_size]; 
    
    AdpcmState rec_state = {0, 0}; // 编码状态

    while (isRecording) {
        // 读取 I2S 数据
        size_t bytes_read = i2s.readBytes(temp_buf, read_size);
        
        if (bytes_read > 0) {
            // PCM (16-bit) -> ADPCM (4-bit)
            // 压缩率 4:1。 bytes_read (PCM字节) -> bytes_read / 4 (ADPCM字节)
            
            size_t sample_count = bytes_read / 2;
            int16_t* pcm_samples = (int16_t*)temp_buf;
            
            // 确保我们有足够的空间
            if (record_data_len + (sample_count / 2) < MAX_RECORD_SIZE) {
                
                for (size_t i = 0; i < sample_count; i += 2) {
                     int16_t s1 = pcm_samples[i];
                     int16_t s2 = (i + 1 < sample_count) ? pcm_samples[i+1] : 0;
                     
                     uint8_t code = adpcm_encode(s1, s2, &rec_state);
                     record_buffer[record_data_len++] = code;
                }
            } else {
                 isRecording = false; // 缓冲满
                 Serial.println("[Audio] Rec Buffer Full!");
            }
        } else {
            vTaskDelay(1);
        }
    }
}

void AppAudio::createWavHeader(uint8_t *header, uint32_t totalDataLen, uint32_t sampleRate, uint8_t sampleBits, uint8_t numChannels) {
    // [ADPCM] 此函数对于 ADPCM 来说已无用，但保留以防万一
}

// ==========================================
//  后台任务具体实现
// ==========================================


void audioPlayTask(void *param) {
    AppAudio *audio = (AppAudio *)param;
    size_t item_size;
    unsigned long last_audio_time = millis();
    bool pa_enabled = false;
    const int PA_TIMEOUT_MS = 2000;
    
    // ADPCM State Reset
    AdpcmState state = {0, 0};

    int16_t out_pcm_stereo[2]; // Stereo sample

    while (1) {
        // 从 RingBuffer 读取 COMPRESSED ADPCM 数据
        // 每次读取一小块，例如 128 bytes
        uint8_t *item = (uint8_t *)xRingbufferReceive(audio->playRingBuf, &item_size, pdMS_TO_TICKS(10));
        
        if (item != NULL) {
            if (!pa_enabled) {
                digitalWrite(PIN_PA_EN, HIGH);
                pa_enabled = true;
                // Reset state on new stream start? 
                // Currently just continuous, maybe slight glitch at start but acceptable
            }
            last_audio_time = millis();

            // Decode Loop
            for (size_t i = 0; i < item_size; i++) {
                uint8_t byte = item[i];
                
                // Sample 1 (Low Nibble)
                int16_t s1 = adpcm_decode(byte & 0x0F, &state);
                out_pcm_stereo[0] = s1; out_pcm_stereo[1] = s1;
                i2s.write((uint8_t*)out_pcm_stereo, 4);

                // Sample 2 (High Nibble)
                int16_t s2 = adpcm_decode((byte >> 4) & 0x0F, &state);
                out_pcm_stereo[0] = s2; out_pcm_stereo[1] = s2;
                i2s.write((uint8_t*)out_pcm_stereo, 4);
            }
            
            vRingbufferReturnItem(audio->playRingBuf, (void *)item);
        } else {
            // Buffer Empty
            if (pa_enabled && (millis() - last_audio_time > PA_TIMEOUT_MS)) {
                digitalWrite(PIN_PA_EN, LOW);
                pa_enabled = false;
                // Reset State when idle
                state.valprev = 0;
                state.index = 0;
            }
            vTaskDelay(1);
        }
    }
}

void playToneTaskWrapper(void *param) {
    ToneParams *p = (ToneParams*)param;
    digitalWrite(PIN_PA_EN, HIGH);
    const int sample_rate = AUDIO_SAMPLE_RATE;
    const int amplitude = 10000; 
    int total_samples = (sample_rate * p->duration) / 1000;
    int16_t sample_buffer[256]; 
    for (int i = 0; i < total_samples; i += 128) {
        int batch = (total_samples - i) > 128 ? 128 : (total_samples - i);
        for (int j = 0; j < batch; j++) {
            int16_t val = (int16_t)(amplitude * sin(2 * PI * p->freq * (i + j) / sample_rate));
            sample_buffer[2*j] = val;     
            sample_buffer[2*j+1] = val;   
        }
        i2s.write((uint8_t*)sample_buffer, batch * 4);
        if(i % 1024 == 0) delay(1);
    }
    // digitalWrite(PIN_PA_EN, LOW); // 交给 audioPlayTask 自动关
    free(p);
    vTaskDelete(NULL); 
}

void recordTaskWrapper(void *param) {
    AppAudio *audio = (AppAudio *)param;
    audio->_recordTask(NULL); 
    vTaskDelete(NULL);
}

// C 接口
void Audio_Play_Click() { MyAudio.playToneAsync(1000, 100); }
void Audio_Record_Start() { MyAudio.startRecording(); }
void Audio_Record_Stop() { MyAudio.stopRecording(); }