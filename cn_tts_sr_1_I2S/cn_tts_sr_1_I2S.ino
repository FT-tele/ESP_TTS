#include <Arduino.h>
#include "ESP_I2S.h"
#include "ESP_SR.h"
#include "esp_tts.h"
#include "esp_partition.h"

// ================== 1. 硬件引脚配置 ==================
#define I2S_PIN_BCK 16
#define I2S_PIN_WS 15
#define I2S_PIN_DOUT 17  // MAX98357 DIN
#define I2S_PIN_DIN 18   // 麦克风 DIN
#define LIGHT_PIN 40

// ================== 2. 全局对象与状态 ==================
I2SClass i2s;
static esp_tts_handle_t tts_handle = NULL;
static TaskHandle_t sr_task_handle = NULL;

extern "C" {
  extern const esp_tts_voice_t esp_tts_voice_xiaole;
}

enum FSM_STATE { STATE_IDLE,
                 STATE_SPEAK };
static volatile FSM_STATE fsm_state = STATE_IDLE;
static volatile bool sr_ready = false;
static volatile bool tts_playing = false;

// ================== 3. SR 命令定义 ==================
enum {
  CMD_OPEN_LIGHT = 0,
  CMD_CLOSE_LIGHT,
};

static const sr_cmd_t sr_commands[] = {
  { CMD_OPEN_LIGHT, "开灯", "kai deng" },
  { CMD_CLOSE_LIGHT, "关灯", "guan deng" },
};

// ================== 4. TTS 核心执行函数 ==================
void execute_tts_logic(const char *text) {
  if (tts_handle == NULL) return;

  if (esp_tts_parse_chinese(tts_handle, (char *)text)) {
    int len[1];
    do {
      // 1. 获取 TTS 原始 PCM 数据 (16bit, 16kHz, Mono 单声道)
      short *pcm = esp_tts_stream_play(tts_handle, len, 4);

      if (len[0] > 0) {
        // 2. 将单声道扩充为双声道 (解决语速快一倍的问题)
        // 申请一个临时缓冲区，大小为原数据的 2 倍
        int stereo_samples = len[0] * 2;
        int16_t *stereo_buffer = (int16_t *)malloc(stereo_samples * sizeof(int16_t));

        if (stereo_buffer != NULL) {
          for (int i = 0; i < len[0]; i++) {
            stereo_buffer[i * 2] = pcm[i];      // 左声道
            stereo_buffer[i * 2 + 1] = pcm[i];  // 右声道
          }

          // 3. 写入双声道数据
          i2s.write((uint8_t *)stereo_buffer, stereo_samples * sizeof(int16_t));

          // 4. 释放临时内存
          free(stereo_buffer);
        }
      }
    } while (len[0] > 0);

    // 播报结束静音，防止 MAX98357 产生爆音
    int16_t silence[512] = { 0 };  // 增加静音长度
    i2s.write((uint8_t *)silence, sizeof(silence));
  }
}

// ================== 5. OrbitWave (履信) 播报流管理 ==================
void orbitwave_speak(const char *text) {
  if (tts_playing) return;
  tts_playing = true;

  Serial0.printf("[TTS] 播报中: %s\n", text);

  // --- A. 暂停 SR 识别 (防止自激) ---
  sr_ready = false;
  if (sr_task_handle) vTaskSuspend(sr_task_handle);
  ESP_SR.setMode(SR_MODE_OFF);

  vTaskDelay(pdMS_TO_TICKS(100));  // 给 SR 留出停止缓冲区
  fsm_state = STATE_SPEAK;

  // --- B. 执行 TTS ---
  execute_tts_logic(text);

  // --- C. 恢复 SR 识别 ---
  vTaskDelay(pdMS_TO_TICKS(200));
  fsm_state = STATE_IDLE;

  if (sr_task_handle) vTaskResume(sr_task_handle);
  sr_ready = true;
  ESP_SR.setMode(SR_MODE_COMMAND);

  tts_playing = false;
  Serial0.println("[SR] 恢复识别模式");
}

// ================== 6. SR 回调逻辑 ==================
void onSrEvent(sr_event_t event, int command_id, int phrase_id) {
  if (!sr_ready) return;

  if (event == SR_EVENT_COMMAND) {
    Serial0.printf("[SR] 识别到命令: %d\n", command_id);

    if (command_id == CMD_OPEN_LIGHT) {
      digitalWrite(LIGHT_PIN, HIGH);
      orbitwave_speak("灯已打开");
    } else if (command_id == CMD_CLOSE_LIGHT) {
      digitalWrite(LIGHT_PIN, LOW);
      orbitwave_speak("灯已关闭");
    }

    // 强制重置识别状态机，确保持续识别
    ESP_SR.setMode(SR_MODE_WAKEWORD);
    ESP_SR.setMode(SR_MODE_COMMAND);
  }
}

// ================== 7. 初始化逻辑 ==================
void setup() {
  Serial0.begin(115200);
  pinMode(LIGHT_PIN, OUTPUT);
  digitalWrite(LIGHT_PIN, LOW);

  // --- 初始化 I2S ---
  // 麦克风通常需要 STEREO 模式启动，MAX98357 会自动从流中取数据
  i2s.setPins(I2S_PIN_BCK, I2S_PIN_WS, I2S_PIN_DOUT, I2S_PIN_DIN);
  if (!i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial0.println("❌ I2S 初始化失败");
    return;
  }

  // --- 初始化 TTS 引擎 ---
  const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_data");
  if (part) {
    esp_partition_mmap_handle_t mmap_handle;
    const void *voicedata_ptr = NULL;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &voicedata_ptr, &mmap_handle) == ESP_OK) {
      esp_tts_voice_t *voice = esp_tts_voice_set_init(&esp_tts_voice_xiaole, (void *)voicedata_ptr);
      tts_handle = esp_tts_create(voice);
      Serial0.println("✅ OrbitWave (履信) TTS 加载成功");
    }
  }

  // --- 初始化 SR ---
  ESP_SR.onEvent(onSrEvent);
  ESP_SR.begin(i2s, sr_commands, sizeof(sr_commands) / sizeof(sr_cmd_t), SR_CHANNELS_STEREO, SR_MODE_COMMAND);
  sr_task_handle = xTaskGetHandle("ESP_SR_task");

  vTaskDelay(pdMS_TO_TICKS(500));
  orbitwave_speak("系统初始化完成");  // 首次开机播报
}

void loop() {
  // 串口调试：通过串口输入文字测试 TTS
  if (Serial0.available() > 0) {
    String input = Serial0.readStringUntil('\n');
    input.trim();
    if (input.length() > 0) {
      orbitwave_speak(input.c_str());
    }
  }
  vTaskDelay(pdMS_TO_TICKS(100));
}