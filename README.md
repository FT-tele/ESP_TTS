chinese SR need modify the two parameter listed below


C:\Users\userName\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.3.0\libraries\ESP_SR\src\esp32-hal-sr.c


  afe_config.wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "hilexin");
   english use hiesp ,chinese use xiaoaitongxue
  afe_config.wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "hiesp");


  char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
  //char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
