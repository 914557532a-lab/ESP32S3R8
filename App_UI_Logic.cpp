#include "App_UI_Logic.h"
#include "App_Display.h" 
//#include "App_4G.h"      
#include "App_Audio.h"   
#include "App_WiFi.h"    
#include <time.h> 
#include <ArduinoJson.h> 
#include "App_Sys.h"
#include "App_IR.h"

AppUILogic MyUILogic;


// [新增] 设置信号值
void AppUILogic::setSignalCSQ(int csq) {
    _cachedCSQ = csq;
}

// --- [核心修复] 适配新的 JSON 结构并防止空指针崩溃 ---
void AppUILogic::handleAICommand(String jsonString) {
    // 1. 解析 JSON
    DynamicJsonDocument doc(1024); // 使用 Dynamic 防止栈溢出
    DeserializationError error = deserializeJson(doc, jsonString);

    if (error) {
        Serial.print("JSON 解析失败: ");
        Serial.println(error.c_str());
        return;
    }

    // 2. 检查是否有 control 字段
    if (!doc.containsKey("control")) {
        Serial.println("[AI] JSON 缺少 control 字段");
        return;
    }

    bool has_command = doc["control"]["has_command"];
    if (!has_command) {
        Serial.println("[AI] 无设备控制指令 (has_command: false)");
        return;
    }

    // 3. 安全读取字段 (增加空指针检查)
    const char* target = doc["control"]["target"]; // "空调", "灯"
    const char* action = doc["control"]["action"]; // "开", "关"
    const char* value  = doc["control"]["value"];  // "25", "高"

    // 防止 NULL 导致 printf 或 strcmp 崩溃
    if (target == nullptr) target = "Unknown";
    if (action == nullptr) action = "Unknown";
    if (value == nullptr)  value = "";

    Serial.printf("[AI] 执行指令: Target=%s, Action=%s, Value=%s\n", target, action, value);

    // 4. 执行具体逻辑 (示例)
    if (strcmp(target, "空调") == 0) {
        if (strcmp(action, "开") == 0) {
             Serial.println(">>> 正在开启空调...");
        } else if (strcmp(action, "关") == 0) {
             Serial.println(">>> 正在关闭空调...");
        } else if (strcmp(action, "调节") == 0) {
             Serial.printf(">>> 调节空调温度至: %s\n", value);
        }

        // [新增] 检查是否有 ir_code 并发送
        if (doc["control"].containsKey("ir_code")) {
            const char* ir_code = doc["control"]["ir_code"];
            if (ir_code && strlen(ir_code) > 0) {
                Serial.printf("[IRQ] 执行红外发送: %s\n", ir_code);
                MyIR.sendQDHSString(String(ir_code));
            }
        }
    } 
    else if (strcmp(target, "灯") == 0) {
        if (strcmp(action, "开") == 0) {
             Serial.println(">>> 开灯");
        } else if (strcmp(action, "关") == 0) {
             Serial.println(">>> 关灯");
        }
    }
}

void AppUILogic::init() {
    _uiGroup = lv_group_create();
    
    // 安全添加对象
    if(ui_ButtonAI) lv_group_add_obj(_uiGroup, ui_ButtonAI);
    if(ui_ButtonLink) lv_group_add_obj(_uiGroup, ui_ButtonLink);
    
    if(ui_ButtonAI) lv_group_focus_obj(ui_ButtonAI);

    configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
    Serial.println("[UI Logic] Init Done.");
}

void AppUILogic::toggleFocus() {
    if (_uiGroup) {
        lv_group_focus_next(_uiGroup);
        MyAudio.playToneAsync(600, 50); 
    }
}

void AppUILogic::showQRCode() {
    String content = "IMEI:865432123456789"; 
    const char* data = content.c_str();
    Serial.printf("[UI] Showing QR: %s\n", data);

    if (_qrObj == NULL && ui_PanelQR) {
        _qrObj = lv_qrcode_create(ui_PanelQR, 110, lv_color_black(), lv_color_white());
        lv_obj_center(_qrObj);
        if(ui_ImageQR) lv_obj_add_flag(ui_ImageQR, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_width(_qrObj, 4, 0);
        lv_obj_set_style_border_color(_qrObj, lv_color_white(), 0);
    }
    if(_qrObj) lv_qrcode_update(_qrObj, data, strlen(data));
}

void AppUILogic::executeLongPressStart() {
    lv_obj_t* focusedObj = lv_group_get_focused(_uiGroup);

    if (focusedObj == ui_ButtonAI) {
        Serial.println("[UI] LongPress: Start Recording");
        
        if(ui_ButtonAI) lv_obj_add_flag(ui_ButtonAI, LV_OBJ_FLAG_HIDDEN);
        if(ui_ButtonLink) lv_obj_add_flag(ui_ButtonLink, LV_OBJ_FLAG_HIDDEN);
        
        MyAudio.startRecording();
        _isRecording = true;

    } else if (focusedObj == ui_ButtonLink) {
        Serial.println("[UI] LongPress: Go to QR");
        MyAudio.playToneAsync(1000, 100);
        if(ui_QRScreen) _ui_screen_change(&ui_QRScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, &ui_QRScreen_screen_init);
        showQRCode();
    }
}

void AppUILogic::sendAudioToPC() {
    if (MyAudio.record_buffer == NULL || MyAudio.record_data_len == 0) {
        Serial.println("[UI] 无录音数据，取消上传");
        finishAIState();
        return;
    }

    NetMessage msg;
    msg.type = NET_EVENT_UPLOAD_AUDIO; 
    msg.len  = 0; 
    msg.data = NULL; 

    if (xQueueSend(NetQueue_Handle, &msg, 0) == pdTRUE) {
        Serial.println("[UI] 已通知网络任务开始处理录音");
        updateAssistantStatus("Sending...");
    } else {
        Serial.println("[UI] 错误：网络队列已满");
        finishAIState();
    }
}

void AppUILogic::executeLongPressEnd() {
    if (_isRecording) {
        Serial.println("[UI] Released: Stop Recording");
        MyAudio.stopRecording();
        _isRecording = false;
        
        sendAudioToPC(); 
    }
}

void AppUILogic::finishAIState() {
    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
        Serial.println("[UI] AI Process Finished. Restoring UI.");

        if(ui_ButtonAI) {
            lv_obj_clear_flag(ui_ButtonAI, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(ui_ButtonAI, lv_color_hex(0xF9F9F9), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        if(ui_ButtonLink) lv_obj_clear_flag(ui_ButtonLink, LV_OBJ_FLAG_HIDDEN);
        
        xSemaphoreGive(xGuiSemaphore);
    }
}

void AppUILogic::updateAssistantStatus(const char* status) {
    if (xSemaphoreTake(xGuiSemaphore, 100) == pdTRUE) { 
        Serial.printf("[UI Status] %s\n", status);
        // 如果有状态 Label，在这里更新
        // if(ui_LabelStatus) lv_label_set_text(ui_LabelStatus, status);
        xSemaphoreGive(xGuiSemaphore);
    }
}

void AppUILogic::showReplyText(const char* text) {
    if (xSemaphoreTake(xGuiSemaphore, 100) == pdTRUE) {
        Serial.printf("[UI Reply] %s\n", text);
        // 如果有回复显示区域，在这里更新
        // if(ui_LabelReply) lv_label_set_text(ui_LabelReply, text);
        xSemaphoreGive(xGuiSemaphore);
    }
}

// 注意这里是 AppUILogic
void AppUILogic::updateStatusBar() {
    if (xSemaphoreTake(xGuiSemaphore, 0) == pdTRUE) {
        if (lv_scr_act() == ui_MainScreen && ui_MainScreen != NULL) {
            
            // 使用缓存的 _cachedCSQ
            int signalPercent = 0;
            if (_cachedCSQ > 0 && _cachedCSQ != 99) {
                signalPercent = map(_cachedCSQ, 0, 31, 0, 100);
                if (signalPercent > 100) signalPercent = 100;
            }
            
            if (ui_Bar4gsignal) {
                lv_bar_set_value(ui_Bar4gsignal, signalPercent, LV_ANIM_ON);
            }

            // 更新时间
            if (ui_LabelTime) {
                struct tm timeinfo;
                if (getLocalTime(&timeinfo, 0)) { 
                    char timeStr[10];
                    sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
                    lv_label_set_text(ui_LabelTime, timeStr);
                }
            }
        }
        xSemaphoreGive(xGuiSemaphore);
    }
}

void AppUILogic::handleInput(KeyAction action) {
    if (action == KEY_NONE) return;

    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
        
        lv_obj_t* currentScreen = lv_scr_act();

        switch (action) {
            case KEY_SHORT_PRESS:
                if (currentScreen == ui_MainScreen) {
                    toggleFocus();
                } 
                else if (currentScreen == ui_QRScreen) {
                    if(ui_MainScreen) _ui_screen_change(&ui_MainScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, &ui_MainScreen_screen_init);
                    if (_qrObj != NULL) {
                        lv_obj_del(_qrObj);
                        _qrObj = NULL;
                    }
                }
                break;

            case KEY_LONG_PRESS_START:
                if (currentScreen == ui_MainScreen) {
                    executeLongPressStart();
                }
                break;

            case KEY_LONG_PRESS_END:
                if (currentScreen == ui_MainScreen) {
                    executeLongPressEnd();
                }
                break;
                
            default: break;
        }
        xSemaphoreGive(xGuiSemaphore);
    }
}

void AppUILogic::loop() {
    static uint32_t lastUpdate = 0;
    if (millis() - lastUpdate > 1000) {
        lastUpdate = millis();
        if (lv_scr_act() == ui_MainScreen) {
            updateStatusBar();
        }
    }
}