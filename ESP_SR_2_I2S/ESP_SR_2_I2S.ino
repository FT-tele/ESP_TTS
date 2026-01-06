#include <Arduino.h>
#include "ESP_I2S.h"
#include "ESP_SR.h"
#include "esp_tts.h"
#include "esp_partition.h"

/*

C:\Users\userName\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.3.0\libraries\ESP_SR\src




  afe_config.wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "hilexin");
   english use hiesp ,chinese use xiaoaitongxue
  afe_config.wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "hiesp");


  char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
  //char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);

*/
// ================== 1. 硬件引脚配置 ==================
// 麦克风 I2S
#define MIC_I2S_NUM I2S_NUM_0
#define MIC_BCK 40
#define MIC_WS 41
#define MIC_DIN 42

// 扬声器 I2S
#define SPEAKER_I2S_NUM I2S_NUM_1
#define SPEAKER_BCK 16
#define SPEAKER_WS 15
#define SPEAKER_DOUT 17

#define LIGHT_PIN 48

// ================== 2. 全局对象与状态 ==================
I2SClass i2s_mic;
I2SClass i2s_speaker;
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
      // 获取 TTS 原始 PCM 数据 (16bit, 16kHz, Mono)
      short *pcm = esp_tts_stream_play(tts_handle, len, 4);

      if (len[0] > 0) {
        int stereo_samples = len[0] * 2;
        int16_t *stereo_buffer = (int16_t *)malloc(stereo_samples * sizeof(int16_t));

        if (stereo_buffer != NULL) {
          for (int i = 0; i < len[0]; i++) {
            stereo_buffer[i * 2] = pcm[i];
            stereo_buffer[i * 2 + 1] = pcm[i];
          }

          // 写入扬声器 I2S
          i2s_speaker.write((uint8_t *)stereo_buffer, stereo_samples * sizeof(int16_t));
          free(stereo_buffer);
        }
      }
    } while (len[0] > 0);

    // 播报结束静音
    int16_t silence[512] = { 0 };
    i2s_speaker.write((uint8_t *)silence, sizeof(silence));
  }
}

// ================== 5. TTS 播报管理 ==================
void orbitwave_speak(const char *text) {
  if (tts_playing) return;
  tts_playing = true;

  Serial0.printf("[TTS] 播报中: %s\n", text);

  // 暂停 SR 识别
  sr_ready = false;
  if (sr_task_handle) vTaskSuspend(sr_task_handle);
  ESP_SR.setMode(SR_MODE_OFF);

  vTaskDelay(pdMS_TO_TICKS(100));
  fsm_state = STATE_SPEAK;

  execute_tts_logic(text);

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

    ESP_SR.setMode(SR_MODE_WAKEWORD);
    ESP_SR.setMode(SR_MODE_COMMAND);
  }
}

// ================== 7. 初始化逻辑 ==================
void setup() {
  Serial0.begin(115200);
  pinMode(LIGHT_PIN, OUTPUT);
  digitalWrite(LIGHT_PIN, LOW);

  // --- 初始化麦克风 I2S ---
  i2s_mic.setPins(MIC_BCK, MIC_WS, -1, MIC_DIN);


  // --- 初始化扬声器 I2S ---
  i2s_speaker.setPins(SPEAKER_BCK, SPEAKER_WS, SPEAKER_DOUT, -1);
  // 初始化麦克风 I2S
  // --- 初始化麦克风 I2S --- 
  if (!i2s_mic.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial0.println("❌ 麦克风 I2S 初始化失败");
  }

  // --- 初始化扬声器 I2S ---
  i2s_speaker.setPins(SPEAKER_BCK, SPEAKER_WS, SPEAKER_DOUT, -1);  // DOUT 输出，DIN=-1
  if (!i2s_speaker.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial0.println("❌ 扬声器 I2S 初始化失败");
  }


  // --- 初始化 TTS ---
  const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_data");
  if (part) {
    esp_partition_mmap_handle_t mmap_handle;
    const void *voicedata_ptr = NULL;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &voicedata_ptr, &mmap_handle) == ESP_OK) {
      esp_tts_voice_t *voice = esp_tts_voice_set_init(&esp_tts_voice_xiaole, (void *)voicedata_ptr);
      tts_handle = esp_tts_create(voice);
      Serial0.println("✅ OrbitWave TTS 加载成功");
    }
  }

  // --- 初始化 SR ---
  ESP_SR.onEvent(onSrEvent);
  ESP_SR.begin(i2s_mic, sr_commands, sizeof(sr_commands) / sizeof(sr_cmd_t), SR_CHANNELS_STEREO, SR_MODE_COMMAND);
  sr_task_handle = xTaskGetHandle("ESP_SR_task");

  vTaskDelay(pdMS_TO_TICKS(500));
  orbitwave_speak("系统初始化完成");
}

// ================== 8. 主循环 ==================
void loop() {
  if (Serial0.available() > 0) {
    String input = Serial0.readStringUntil('\n');
    input.trim();
    if (input.length() > 0) {
      orbitwave_speak(input.c_str());
    }
  }
  vTaskDelay(pdMS_TO_TICKS(100));
}
