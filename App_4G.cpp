#include "App_4G.h"
#include <Arduino.h>

App4G My4G;

// 定义 RingBuffer 大小 (8KB 足够缓冲音频流)
#define RX_BUF_SIZE 8192
static uint8_t rxBuf[RX_BUF_SIZE];
static volatile int rxHead = 0;
static volatile int rxTail = 0;

void App4G::init() {
    pinMode(PIN_4G_PWR, OUTPUT);
    digitalWrite(PIN_4G_PWR, LOW); 
}

// [修改] 增加调试回显
bool App4G::waitResponse(const char* expected, uint32_t timeout) {
    unsigned long start = millis();
    String recv = "";
    while (millis() - start < timeout) {
        if (_serial4G->available()) {
            char c = _serial4G->read();
            
            Serial.write(c); 
            
            recv += c;
            if (recv.indexOf(expected) != -1) return true;
        }
        vTaskDelay(1);
    }
    return false;
}

bool App4G::checkBaudrate(uint32_t baud) {
    _serial4G->updateBaudRate(baud);
    delay(50);
    // Clear buffer
    while (_serial4G->available()) _serial4G->read(); 
    
    _serial4G->print("AT\r\n");
    
    // Custom wait response for baud check
    unsigned long start = millis();
    String recv = "";
    while (millis() - start < 500) { // Increased timeout
        if (_serial4G->available()) {
            char c = _serial4G->read();
            Serial.write(c); // Debug echo
            recv += c;
            if (recv.indexOf("OK") != -1) return true;
            if (recv.indexOf("ERROR") != -1) return true; // ERROR means baud is good, just command failed
            if (recv.indexOf("AT") != -1 && recv.length() > 4) {
                 // We saw AT echo + something else, likely good
                 return true;
            }
        }
        vTaskDelay(1);
    }
    return false;
}

void App4G::powerOn() {
    digitalWrite(PIN_4G_PWR, HIGH); 
    // Give it some time to power up physically
    delay(2000); 

    // Initialize Serial Port
    _serial4G->begin(115200, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
    delay(100);

    uint32_t targetBaud = 115200; 
    bool synced = false;

    Serial.println("[4G] Powering On & Syncing...");

    // 尝试同步波特率 (最多尝试 5 次循环)
    for (int i = 0; i < 5; i++) {
        Serial.printf("[4G] Sync Attempt %d...\n", i + 1);

        // 1. 尝试 115200 (默认/目标)
        if (checkBaudrate(115200)) {
            Serial.println("[4G] Synced at 115200!");
            synced = true;
            break;
        }

        // 2. 尝试 921600 (之前可能被修改过)
        if (checkBaudrate(921600)) {
            Serial.println("[4G] Found at 921600, resetting to 115200...");
            _serial4G->print("AT+IPR=115200\r\n"); 
            delay(500); // 等待生效
            // 切回 115200 验证
            if (checkBaudrate(115200)) {
                synced = true;
                break;
            }
        }
        
        // 3. 尝试 460800
         if (checkBaudrate(460800)) {
            Serial.println("[4G] Found at 460800, resetting to 115200...");
            _serial4G->print("AT+IPR=115200\r\n"); 
            delay(500);
            if (checkBaudrate(115200)) {
                synced = true;
                break;
            }
        }
    }

    // 如果还没有同步成功，尝试硬件复位
    if (!synced) {
        Serial.println("[4G] Sync failed. Performing Hard Reset...");
        pinMode(PIN_4G_PWRKEY, OUTPUT);
        digitalWrite(PIN_4G_PWRKEY, HIGH); delay(100);
        digitalWrite(PIN_4G_PWRKEY, LOW);  delay(2500); // 拉低至少 2s 关机/复位
        digitalWrite(PIN_4G_PWRKEY, HIGH); pinMode(PIN_4G_PWRKEY, INPUT);
        
        // 复位后等待模块重启 (至少 10s)
        Serial.println("[4G] Waiting for reboot...");
        delay(10000); 
        
        // 复位后再次尝试 115200
         if (checkBaudrate(115200)) {
             synced = true;
         } else {
             // 如果复位后还不行，可能是模块处于自动波特率模式，多发几个 AT 激活
             _serial4G->updateBaudRate(115200);
             for(int k=0; k<10; k++) {
                 _serial4G->print("AT\r\n"); // Keep trying
                 delay(500); // Wait longer
                 if(_serial4G->available()) {
                     synced = true;
                     break;
                 }
             }
         }
    }

    if (synced) {
        Serial.println("[4G] Module Ready (Signal Detected). Stabilizing...");
        
        _serial4G->updateBaudRate(115200);
        
        // 尝试“打通” AT 指令，直到收到 OK
        bool at_ok = false;
        for (int i=0; i<10; i++) {
            _serial4G->print("AT\r\n");
            delay(100);
            if (waitResponse("OK", 500)) {
                at_ok = true;
                break;
            }
            // 清空多余的字符
            while(_serial4G->available()) Serial.write(_serial4G->read());
        }
        
        if (at_ok) {
            Serial.println("[4G] AT Command OK!");
            _serial4G->println("ATE0"); // 关闭回显
            waitResponse("OK", 500); 
        } else {
            Serial.println("[4G] Warning: Module responding but AT not OK.");
        }
    } else {
        Serial.println("[4G] CRITICAL: Module not responding!");
    }

    if (_modem == nullptr) _modem = new TinyGsm(*_serial4G);
    if (_client == nullptr) _client = new TinyGsmClient(*_modem);
}

// [修复] 真正的连接检查逻辑
bool App4G::connect(unsigned long timeout_ms) {
    unsigned long start = millis();
    Serial.print("[4G] Checking Network... ");
    
    while (millis() - start < timeout_ms) {
        // 使用 TinyGsm 检查网络注册状态
        if (_modem && _modem->isNetworkConnected()) {
            Serial.println("OK! (Connected)");
            return true;
        }
        
        Serial.print(".");
        vTaskDelay(1000); 
    }
    
    Serial.println(" Timeout!");
    return false;
}

bool App4G::connectTCP(const char* host, uint16_t port) {
    rxHead = rxTail = 0; 
    g_st = ST_SEARCH;    
    
    // 1. 关闭可能存在的旧连接
    _serial4G->println("AT+MIPCLOSE=1"); 
    waitResponse("OK", 500);    

    // (防止 AT+MIPCALL=1 之后过一会掉线)
    _serial4G->println("AT+MIPCALL?");
    // 如果回复 +MIPCALL: 0 说明没 IP
    if (waitResponse("+MIPCALL: 0", 500)) {
         Serial.println("[4G] Reactivating IP...");
         _serial4G->println("AT+CGDCONT=1,\"IP\",\"cmnet\""); waitResponse("OK", 500);
         _serial4G->println("AT+MIPCALL=1"); 
         waitResponse("OK", 3000);
    }

    // 3. 发起连接
    _serial4G->printf("AT+MIPOPEN=1,0,\"%s\",%d,0\r\n", host, port);
    
    // 4. [核心修复] 智能等待连接结果
    // 只要收到 "+MIPOPEN: 1,1" 或者 "CONNECT" 都算成功
    unsigned long start = millis();
    while (millis() - start < 20000) {
        if (_serial4G->available()) {
            String line = _serial4G->readStringUntil('\n');
            line.trim();
            
            // 打印调试信息，让你看到发生了什么
            if (line.length() > 0) Serial.println("[4G RAW] " + line);
            
            // 判定成功条件：
            // 条件A: 出现 CONNECT (某些固件)
            // 条件B: 出现 +MIPOPEN: 1,1 (你的固件)
            if (line.indexOf("CONNECT") != -1) return true;
            if (line.indexOf("+MIPOPEN:") != -1 && line.indexOf(",1") != -1) return true;
            
            // 判定失败条件
            if (line.indexOf("ERROR") != -1) return false;
            if (line.indexOf("+MIPOPEN:") != -1 && line.indexOf(",0") != -1) return false;
        }
        vTaskDelay(10);
    }
    
    Serial.println("[4G] TCP Connect Timeout");
    return false;
}

void App4G::closeTCP() {
    _serial4G->println("AT+MIPCLOSE=1");
    waitResponse("OK", 2000);
}

bool App4G::sendData(const uint8_t* data, size_t len) {
    _serial4G->printf("AT+MIPSEND=1,%d\r\n", len);
    if (!waitResponse(">", 5000)) return false;
    _serial4G->write(data, len);
    return waitResponse("OK", 10000); 
}

bool App4G::sendData(uint8_t* data, size_t len) {
    return sendData((const uint8_t*)data, len);
}

// ==========================================================
// 核心：高性能状态机 (无 String, 纯字符匹配)
// ==========================================================
// 目标匹配模式: "+MIPRTCP:"
static const char* HEADER_MATCH = "+MIPRTCP:";
static int matchIdx = 0;
static char lenBuf[16]; 
static int lenBufIdx = 0;
static int dataBytesLeft = 0;

void App4G::process4GStream() {
    while (_serial4G->available()) {
        char c = _serial4G->read();

        switch (g_st) {
            case ST_SEARCH: 
                // 逐字匹配 "+MIPRTCP:"
                if (c == HEADER_MATCH[matchIdx]) {
                    matchIdx++;
                    if (HEADER_MATCH[matchIdx] == '\0') {
                        // 匹配成功!
                        g_st = ST_SKIP_ID;
                        matchIdx = 0;
                    }
                } else {
                    // 匹配失败，回退
                    if (c == HEADER_MATCH[0]) matchIdx = 1;
                    else matchIdx = 0;
                }
                break;

            case ST_SKIP_ID:
                // 跳过 " 1," 这样的ID部分，直到遇到逗号
                if (c == ',') {
                    g_st = ST_READ_LEN;
                    lenBufIdx = 0;
                    memset(lenBuf, 0, sizeof(lenBuf));
                }
                break;

            case ST_READ_LEN:
                // 读取长度数字，直到遇到下一个逗号
                if (c == ',') {
                    lenBuf[lenBufIdx] = '\0';
                    dataBytesLeft = atoi(lenBuf); // 解析长度
                    if (dataBytesLeft > 0) {
                        g_st = ST_READ_DATA;
                    } else {
                        g_st = ST_SEARCH; 
                    }
                } else if (isDigit(c)) {
                    if (lenBufIdx < 10) { // 防止溢出
                        lenBuf[lenBufIdx++] = c;
                    }
                }
                break;

            case ST_READ_DATA:
                if (dataBytesLeft > 0) {
                    // 存入环形缓冲区
                    int next = (rxHead + 1) % RX_BUF_SIZE;
                    if (next != rxTail) {
                        rxBuf[rxHead] = (uint8_t)c;
                        rxHead = next;
                    }
                    dataBytesLeft--;
                }
                
                if (dataBytesLeft == 0) {
                    g_st = ST_SEARCH; // 收完这一包，继续找下一包
                    matchIdx = 0;
                }
                break;
        }
    }
}

int App4G::popCache() {
    if (rxHead == rxTail) return -1;
    uint8_t c = rxBuf[rxTail];
    rxTail = (rxTail + 1) % RX_BUF_SIZE;
    return c;
}

int App4G::readData(uint8_t* buf, size_t wantLen, uint32_t timeout_ms) {
    unsigned long start = millis();
    size_t received = 0;
    while (received < wantLen && (millis() - start < timeout_ms)) {
        process4GStream(); // 搬运数据
        
        int b = popCache();
        if (b != -1) {
            buf[received++] = (uint8_t)b;
            start = millis(); // 收到数据刷新超时
            
            // 极速模式下，减少 vTaskDelay 频率
            if (received % 256 == 0) vTaskDelay(1); 
        } else {
            vTaskDelay(1); // 没数据时休息一下
        }
    }
    return received;
}

// 简单的 Getter 包装
bool App4G::isConnected() { return _modem && _modem->isNetworkConnected(); }
String App4G::getIMEI() { return _modem ? _modem->getIMEI() : ""; }
TinyGsmClient& App4G::getClient() { return *_client; }
void App4G::sendRawAT(String cmd) { _serial4G->println(cmd); }
int App4G::getSignalCSQ() { return _modem ? _modem->getSignalQuality() : 0; }