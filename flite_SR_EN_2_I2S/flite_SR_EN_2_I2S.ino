#include <Arduino.h>
#include "ESP_I2S.h"
#include "ESP_SR.h"


/*

C:\Users\userName\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.3.0\libraries\ESP_SR\src




  afe_config.wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "hilexin");
   english use hiesp ,chinese use xiaoaitongxue
  afe_config.wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "hiesp");


  char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
  //char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);

*/
extern "C" {
  #include "flite.h"
  cst_voice *register_cmu_us_kal();
}

// ================== 硬件配置 ==================
#define MIC_BCK   40
#define MIC_WS    41
#define MIC_DIN   42   // 麦克风 DIN
#define SPEAKER_BCK 16
#define SPEAKER_WS  15
#define SPEAKER_DOUT 17

#define LIGHT_PIN  40
#define FAN_PIN    41

// ================== FSM 状态 ==================
enum FSM_STATE { STATE_IDLE, STATE_SPEAK };
static volatile FSM_STATE fsm_state = STATE_SPEAK;  // 开机 TTS 播放期间
static volatile bool sr_ready = false;

// ================== I2S ==================
I2SClass i2s_mic;
I2SClass i2s_speaker;

// ================== Flite ==================
static cst_voice *voice = nullptr;
static volatile bool tts_playing = false;

// ================== SR 命令 ==================
enum {
  CMD_LIGHT_ON = 0,
  CMD_LIGHT_OFF,
  CMD_FAN_ON,
  CMD_FAN_OFF
};

static const sr_cmd_t sr_commands[] = {
  { CMD_LIGHT_ON,  "Turn on the light",  "TkN nN jc LiT" },
  { CMD_LIGHT_OFF, "Turn off the light", "TkN eF jc LiT" },
  { CMD_FAN_ON,    "Start fan",          "STnRT FaN" },
  { CMD_FAN_OFF,   "Stop fan",           "STnP FaN" }
};

// ================== SR Task Handle ==================
static TaskHandle_t sr_task_handle = NULL;

// ============================================================
//   8k → 16k 线性插值重采样
// ============================================================
static inline void upsample_8k_to_16k(const int16_t *in, int in_samples, int16_t *out, int *out_samples)
{
  int o = 0;
  for (int i = 0; i < in_samples - 1; i++) {
    out[o++] = in[i];
    out[o++] = (in[i] + in[i+1]) >> 1;
  }
  out[o++] = in[in_samples - 1];
  out[o++] = in[in_samples - 1];
  *out_samples = o;
}

// ============================================================
//   Flite 音频回调
// ============================================================
static int flite_audio_callback(const cst_wave *w, int start, int size, int last, cst_audio_streaming_info_struct *asi)
{
  (void)asi;

  static int16_t up_buf[2048*2];
  int out_samples = 0;

  upsample_8k_to_16k(&w->samples[start], size, up_buf, &out_samples);
  i2s_speaker.write((uint8_t*)up_buf, out_samples * sizeof(int16_t));

  if (last) tts_playing = false;
  return 0;
}

// ================== 工具函数 ==================
void i2s_flush_tx() {
  int16_t zero[256] = {0};
  i2s_speaker.write((uint8_t*)zero, sizeof(zero));
}

void fsm_set(FSM_STATE next) {
  fsm_state = next;
  Serial0.printf("[FSM] State changed: %s\n", next==STATE_IDLE?"IDLE":"SPEAK");
}

// ================== TTS 播放 ==================
void flite_tts_play(const char *text)
{
  if (tts_playing) return;
  tts_playing = true;

  Serial0.printf("[TTS] Play start: %s\n", text);

  // 暂停 SR
  sr_ready = false;
  if (sr_task_handle) vTaskSuspend(sr_task_handle);
  ESP_SR.setMode(SR_MODE_OFF);
  vTaskDelay(pdMS_TO_TICKS(200));

  i2s_flush_tx();
  fsm_set(STATE_SPEAK);

  cst_wave *wave = flite_text_to_wave(text, voice);
  delete_wave(wave);

  i2s_flush_tx();
  vTaskDelay(pdMS_TO_TICKS(150));

  fsm_set(STATE_IDLE);
  vTaskDelay(pdMS_TO_TICKS(300));

  // 恢复 SR
  if (sr_task_handle) vTaskResume(sr_task_handle);
  sr_ready = true;
  ESP_SR.setMode(SR_MODE_COMMAND);

  Serial0.println("[TTS] Play end");
  Serial0.println("[SR] Command mode resumed");

  tts_playing = false;
}

// ================== SR 回调 ==================
void onSrEvent(sr_event_t event, int cmd, int phrase) {
  Serial0.printf("[SR] Event: %d, cmd: %d, phrase: %d\n", event, cmd, phrase);

  if (!sr_ready) return;
  if (cmd < 0 || event != SR_EVENT_COMMAND) {
    ESP_SR.setMode(SR_MODE_OFF);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_SR.setMode(SR_MODE_COMMAND);
    Serial0.println("[SR] FSM reset after invalid command");
    return;
  }

  if (event == SR_EVENT_COMMAND) {
    switch (cmd) {
      case CMD_LIGHT_ON:
        digitalWrite(LIGHT_PIN, HIGH);
        flite_tts_play("Light turned on");
        break;
      case CMD_LIGHT_OFF:
        digitalWrite(LIGHT_PIN, LOW);
        flite_tts_play("Light turned off");
        break;
      case CMD_FAN_ON:
        digitalWrite(FAN_PIN, HIGH);
        flite_tts_play("Fan started");
        break;
      case CMD_FAN_OFF:
        digitalWrite(FAN_PIN, LOW);
        flite_tts_play("Fan stopped");
        break;
    }
  }
}

// ================== Setup ==================
void setup() {
  Serial0.begin(115200);
  delay(500);

  pinMode(LIGHT_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(LIGHT_PIN, LOW);
  digitalWrite(FAN_PIN, LOW);

  // ----------------- 麦克风 I2S 初始化 -----------------
  i2s_mic.setPins(MIC_BCK, MIC_WS, -1, MIC_DIN);
  if (!i2s_mic.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial0.println("❌ 麦克风 I2S 初始化失败");
  }

  // ----------------- 扬声器 I2S 初始化 -----------------
  i2s_speaker.setPins(SPEAKER_BCK, SPEAKER_WS, SPEAKER_DOUT, -1);
  if (!i2s_speaker.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial0.println("❌ 扬声器 I2S 初始化失败");
  }

  // ----------------- Flite 初始化 -----------------
  flite_init();
  voice = register_cmu_us_kal();
  cst_audio_streaming_info *asi = new_audio_streaming_info();
  asi->asc = flite_audio_callback;
  asi->userdata = nullptr;
  feat_set(voice->features, "streaming_info", audio_streaming_info_val(asi));

  Serial0.println("[Flite] ready");
  vTaskDelay(pdMS_TO_TICKS(500));
  flite_tts_play("System boot completed");

  // ----------------- SR 初始化 -----------------
  ESP_SR.onEvent(onSrEvent);
  ESP_SR.begin(i2s_mic, sr_commands, sizeof(sr_commands)/sizeof(sr_cmd_t),
               SR_CHANNELS_STEREO, SR_MODE_COMMAND);

  sr_task_handle = xTaskGetHandle("ESP_SR_task");
  fsm_set(STATE_IDLE);
  sr_ready = true;
  Serial0.println("[SR] Ready for command recognition");
}

// ================== Loop ==================
void loop() {
  vTaskDelay(pdMS_TO_TICKS(10));
}
