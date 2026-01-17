/**
 * @file App_Server.cpp
 * @brief 服务端交互逻辑 (方案二：全缓冲下载模式 - 修复编译报错版)
 */
#include "App_Server.h"
#include "App_WiFi.h"
#include "App_4G.h"
#include "App_Audio.h"
#include "App_UI_Logic.h"
#include "App_Sys.h"
#include "Arduino.h"

AppServer MyServer;

// ==========================================
//  辅助函数
// ==========================================

// 将 Hex 字符转换为数值
uint8_t hexCharToVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

// 4G 模式下手动发送整数
bool sendIntManual(uint32_t val) {
    uint8_t buf[4];
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8)  & 0xFF;
    buf[3] = (val)       & 0xFF;
    return My4G.sendData(buf, 4);
}

// ==========================================
//  类成员函数实现
// ==========================================

// [核心] 必须实现 init 函数，否则连接器找不到符号
void AppServer::init(const char* ip, uint16_t port) {
    this->_server_ip = ip;
    this->_server_port = port;
}

void AppServer::chatWithServer(Client* networkClient) {
    bool isWiFi = MyWiFi.isConnected(); 
    Serial.printf("[Server] Connect %s:%d (%s)...\n", _server_ip, _server_port, isWiFi?"WiFi":"4G");
    MyUILogic.updateAssistantStatus("连接中...");

    // 1. 建立连接
    bool connected = false;
    if (isWiFi) { 
        connected = networkClient->connect(_server_ip, _server_port);
    } else { 
        connected = My4G.connectTCP(_server_ip, _server_port);
    }

    if (!connected) {
        Serial.println("[Server] Connection Failed!");
        MyUILogic.updateAssistantStatus("服务器连不上");
        vTaskDelay(2000);
        if (!isWiFi) My4G.closeTCP();
        MyUILogic.finishAIState();
        return;
    }

    // 2. 发送录音数据
    uint32_t audioSize = MyAudio.record_data_len;
    MyUILogic.updateAssistantStatus("发送指令...");
    
    if (isWiFi) { 
        // 发送长度头 (Big Endian)
        uint8_t lenBuf[4];
        lenBuf[0] = (audioSize >> 24) & 0xFF;
        lenBuf[1] = (audioSize >> 16) & 0xFF;
        lenBuf[2] = (audioSize >> 8)  & 0xFF;
        lenBuf[3] = (audioSize)       & 0xFF;
        networkClient->write(lenBuf, 4);
        // 发送本体
        networkClient->write(MyAudio.record_buffer, audioSize);
        networkClient->flush(); 
    } 
    else {
        // 4G 发送
        delay(200);
        if(!sendIntManual(audioSize)) { 
            My4G.closeTCP(); 
            MyUILogic.finishAIState();
            return; 
        }
        
        size_t sent = 0;
        while(sent < audioSize) {
            size_t chunk = 1024;
            if(audioSize - sent < chunk) chunk = audioSize - sent;
            if(!My4G.sendData(MyAudio.record_buffer + sent, chunk)) break;
            sent += chunk;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    
    MyUILogic.updateAssistantStatus("思考中...");

    // 3. 接收并解析 (新版：JOSN Hex -> Audio Binary Stream)
    
    String jsonHex = "";
    bool jsonDone = false;
    uint32_t totalBytesReceived = 0;
    
    Serial.println("[Server] Waiting for response...");
    
    // 缓冲区 (ADPCM块) 
    uint8_t streamBuf[256]; 
    unsigned long lastDataTime = millis();
    
    while (1) {
        // 检查连接是否断开 (EOF)
        if (isWiFi) {
            if (!networkClient->connected() && !networkClient->available()) break;
        } else {
            // 4G 在极速模式下如何判断断开？ 
            // 通常依靠超时或者 AT 指令判断
            // 这里我们依靠数据流超时
            if (millis() - lastDataTime > 5000) {
                Serial.println("[Server] Stream Timeout.");
                break;
            }
        }

        // 读取数据
        size_t r = 0;
        if (isWiFi) {
            if (networkClient->available()) {
                 r = networkClient->read(streamBuf, sizeof(streamBuf));
            }
        } else {
            // 4G 读取 (如果缓冲区空则返回0)
            int val = My4G.popCache(); // 先查缓存
            if (val != -1) {
                streamBuf[0] = (uint8_t)val;
                r = 1;
                // 贪婪读取后续
                while(r < sizeof(streamBuf)) {
                    int v2 = My4G.popCache();
                    if(v2 == -1) break;
                    streamBuf[r++] = (uint8_t)v2;
                }
            } else {
                 My4G.process4GStream(); // 触发泵
            }
        }

        if (r > 0) {
            lastDataTime = millis();
            
            // --- 分发数据 ---
            for (size_t i = 0; i < r; i++) {
                uint8_t c = streamBuf[i];
                
                if (!jsonDone) {
                    // Phase 1: JSON (Hex + '*')
                    if (c == '*') {
                        jsonDone = true;
                        Serial.println("\n[Protocol] JSON End. Start Audio Stream...");
                        MyUILogic.updateAssistantStatus("正在回复"); // 收到 * 立刻显示正在回复
                        
                        // Parse JSON
                        if (jsonHex.length() > 0) {
                            int jLen = jsonHex.length() / 2;
                            char* jBuf = (char*)malloc(jLen + 1);
                            if (jBuf) {
                                for (int k=0; k<jLen; k++) jBuf[k] = (hexCharToVal(jsonHex[k*2]) << 4) | hexCharToVal(jsonHex[k*2+1]);
                                jBuf[jLen] = 0;
                                Serial.printf("[Protocol] JSON: %s\n", jBuf);
                                MyUILogic.handleAICommand(String(jBuf));
                                free(jBuf);
                            }
                        }
                    } 
                    else if (c != '\n' && c != '\r') {
                        jsonHex += (char)c;
                    }
                } else {
                    // Phase 2: Audio (Raw Binary ADPCM)
                    // 直接推入播放缓冲 -> 后台解码任务会自动消费
                    // 这里不需要做任何处理，直接是 ADPCM 字节
                    MyAudio.pushToPlayBuffer(&c, 1);
                    totalBytesReceived++;
                    if (totalBytesReceived % 1024 == 0) Serial.print(".");
                }
            }
        } else {
             vTaskDelay(1);
        }
    }

    Serial.printf("\n[Server] Stream Finished. Total: %d bytes.\n", totalBytesReceived);

    // 等待播放缓冲排空 (给一点时间让 tail 追上 head)
    // 但不要死等，因为 playAudioTask 会一直运行
    vTaskDelay(500);

    // 4. 断开连接
    if (isWiFi) networkClient->stop();
    else My4G.closeTCP();
    
    MyUILogic.finishAIState();
}