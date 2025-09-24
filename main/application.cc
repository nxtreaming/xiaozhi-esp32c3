#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "ml307_ssl_transport.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "font_awesome_symbols.h"
#include "iot/thing_manager.h"
#include "assets/lang_config.h"
#include "YT_UART.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <esp_app_desc.h>
#include <driver/uart.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_tls.h>
#include <esp_crt_bundle.h>
#include "YT_UART.h"
#include "gif_test.h"
#define TAG "Application"

static const char *const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "fatal_error",
    "invalid_state"};

Application::Application()
{
    event_group_ = xEventGroupCreate();
    background_task_ = new BackgroundTask(4096 * 8);

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void *arg)
        {
            Application *app = (Application *)arg;
            app->OnClockTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application()
{
    if (clock_timer_handle_ != nullptr)
    {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (background_task_ != nullptr)
    {
        delete background_task_;
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckNewVersion()
{
    auto &board = Board::GetInstance();
    auto display = board.GetDisplay();
    // Check if there is a new firmware version available
    ota_.SetPostData(board.GetJson());

    const int MAX_RETRY = 10;
    int retry_count = 0;

    while (true)
    {
        if (!ota_.CheckVersion())
        {
            retry_count++;
            if (retry_count >= MAX_RETRY)
            {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }
            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", 60, retry_count, MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }
        retry_count = 0;

        // We disable OTA upgrade in develop phase
        if (false && ota_.HasNewVersion())
        {
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "happy", Lang::Sounds::P3_UPGRADE);
            // Wait for the chat state to be idle
            do
            {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } while (GetDeviceState() != kDeviceStateIdle);

            // Use main task to do the upgrade, not cancelable
            Schedule([this, display]()
                     {
                SetDeviceState(kDeviceStateUpgrading);

                display->SetIcon(FONT_AWESOME_DOWNLOAD);
                std::string message = std::string(Lang::Strings::NEW_VERSION) + ota_.GetFirmwareVersion();
                display->SetChatMessage("system", message.c_str());

                auto& board = Board::GetInstance();
                board.SetPowerSaveMode(false);
#if CONFIG_USE_WAKE_WORD_DETECT
                wake_word_detect_.StopDetection();
#endif
                // 预先关闭音频输出，避免升级过程有音频操作
                auto codec = board.GetAudioCodec();
                codec->EnableInput(false);
                codec->EnableOutput(false);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    audio_decode_queue_.clear();
                }
                background_task_->WaitForCompletion();
                delete background_task_;
                background_task_ = nullptr;
                vTaskDelay(pdMS_TO_TICKS(1000));

                ota_.StartUpgrade([display](int progress, size_t speed) {
                    char buffer[64];
                    snprintf(buffer, sizeof(buffer), "%d%% %zuKB/s", progress, speed / 1024);
                    display->SetChatMessage("system", buffer);
                });

                // If upgrade success, the device will reboot and never reach here
                display->SetStatus(Lang::Strings::UPGRADE_FAILED);
                ESP_LOGI(TAG, "Firmware upgrade failed...");
                vTaskDelay(pdMS_TO_TICKS(3000));
                Reboot(); });

            return;
        }

        // No new version, mark the current version as valid
        ota_.MarkCurrentVersionValid();
        std::string message = std::string(Lang::Strings::VERSION) + ota_.GetCurrentVersion();
        display->ShowNotification(message.c_str());

        if (ota_.HasActivationCode())
        {
            // Activation code is valid
            SetDeviceState(kDeviceStateActivating);
            ShowActivationCode();

            // Check again in 60 seconds or until the device is idle
            for (int i = 0; i < 60; ++i)
            {
                if (device_state_ == kDeviceStateIdle)
                {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            continue;
        }

        SetDeviceState(kDeviceStateIdle);
        display->SetChatMessage("system", "");
        // PlaySound(Lang::Sounds::P3_SUCCESS);
        // Exit the loop if upgrade or idle
        break;
    }
}

void Application::ShowActivationCode()
{
    auto &message = ota_.GetActivationMessage();
    auto &code = ota_.GetActivationCode();

    struct digit_sound
    {
        char digit;
        const std::string_view &sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{digit_sound{'0', Lang::Sounds::P3_0},
                                                           digit_sound{'1', Lang::Sounds::P3_1},
                                                           digit_sound{'2', Lang::Sounds::P3_2},
                                                           digit_sound{'3', Lang::Sounds::P3_3},
                                                           digit_sound{'4', Lang::Sounds::P3_4},
                                                           digit_sound{'5', Lang::Sounds::P3_5},
                                                           digit_sound{'6', Lang::Sounds::P3_6},
                                                           digit_sound{'7', Lang::Sounds::P3_7},
                                                           digit_sound{'8', Lang::Sounds::P3_8},
                                                           digit_sound{'9', Lang::Sounds::P3_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);
    vTaskDelay(pdMS_TO_TICKS(1000));
    background_task_->WaitForCompletion();

    for (const auto &digit : code)
    {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound &ds)
                               { return ds.digit == digit; });
        if (it != digit_sounds.end())
        {
            PlaySound(it->sound);
        }
    }
}
uint8_t emoil_cmd[6] = {0x55, 0xAA, 0xC1, 0x01, 0x00, 0xC1};

void Application::SendEmotion(uint8_t type)
{
    emoil_cmd[4] = type;
    emoil_cmd[5] = (0x55 + 0xAA + 0xC1 + 0X01 + type) & 0XFF;
    uart_write_bytes(UART_NUM_1, &emoil_cmd, 6);
}
void Application::SendEmotionByString(const char *emotion)
{
    struct Emotion
    {
        const char *icon;
        const char *text;
        const uint8_t type;
    };

    static const std::vector<Emotion> emotions = {
        {FONT_AWESOME_EMOJI_NEUTRAL, "neutral", EMOIJ_TYPE_NEUTRAL},
        {FONT_AWESOME_EMOJI_HAPPY, "happy", EMOIJ_TYPE_HAPPY},
        {FONT_AWESOME_EMOJI_LAUGHING, "laughing", EMOIJ_TYPE_LAUGHING},
        {FONT_AWESOME_EMOJI_FUNNY, "funny", EMOIJ_TYPE_FUNNY},
        {FONT_AWESOME_EMOJI_SAD, "sad", EMOIJ_TYPE_SAD},
        {FONT_AWESOME_EMOJI_ANGRY, "angry", EMOIJ_TYPE_ANGRY},
        {FONT_AWESOME_EMOJI_CRYING, "crying", EMOIJ_TYPE_CRYING},
        {FONT_AWESOME_EMOJI_LOVING, "loving", EMOIJ_TYPE_LOVING},
        {FONT_AWESOME_EMOJI_EMBARRASSED, "embarrassed", EMOIJ_TYPE_EMBARRASSED},
        {FONT_AWESOME_EMOJI_SURPRISED, "surprised", EMOIJ_TYPE_SURPRISED},
        {FONT_AWESOME_EMOJI_SHOCKED, "shocked", EMOIJ_TYPE_SHOCKED},
        {FONT_AWESOME_EMOJI_THINKING, "thinking", EMOIJ_TYPE_THINKING},
        {FONT_AWESOME_EMOJI_WINKING, "winking", EMOIJ_TYPE_WINKING},
        {FONT_AWESOME_EMOJI_COOL, "cool", EMOIJ_TYPE_COOL},
        {FONT_AWESOME_EMOJI_RELAXED, "relaxed", EMOIJ_TYPE_RELAXED},
        {FONT_AWESOME_EMOJI_DELICIOUS, "delicious", EMOIJ_TYPE_DELICIOUS},
        {FONT_AWESOME_EMOJI_KISSY, "kissy", EMOIJ_TYPE_KISSY},
        {FONT_AWESOME_EMOJI_CONFIDENT, "confident", EMOIJ_TYPE_CONFIDENT},
        {FONT_AWESOME_EMOJI_SLEEPY, "sleepy", EMOIJ_TYPE_SLEEPY},
        {FONT_AWESOME_EMOJI_SILLY, "silly", EMOIJ_TYPE_SILLY},
        {FONT_AWESOME_EMOJI_CONFUSED, "confused", EMOIJ_TYPE_CONFUSED}};

    // 查找匹配的表情
    std::string_view emotion_view(emotion);
    auto it = std::find_if(emotions.begin(), emotions.end(),
                           [&emotion_view](const Emotion &e)
                           { return e.text == emotion_view; });
    // 如果找到匹配的表情就发送对应的类型，否则发送默认的 neutral 类型
    if (it != emotions.end())
    {
        SendEmotion(it->type);
        printf("表情类型 it->type=%d\n", it->type);
    }
    else
    {
        SendEmotion(EMOIJ_TYPE_NEUTRAL); // 默认发送 neutral 类型
    }
}
void Application::Alert(const char *status, const char *message, const char *emotion, const std::string_view &sound)
{
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    // SendEmotionByString(emotion);
    if (!sound.empty())
    {
        PlaySound(sound);
    }
}

void Application::DismissAlert()
{
    if (device_state_ == kDeviceStateIdle)
    {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
        // SendEmotionByString("sleepy");
    }
}

void Application::PlaySound(const std::string_view &sound)
{
    // std::lock_guard<std::mutex> lock(mutex_);  // 确保整个函数加锁
    // opus_decoder_->ResetState();  // 重置解码器状态
    // audio_decode_queue_.clear();  // 清空旧数据
    auto codec = Board::GetInstance().GetAudioCodec();
    codec->EnableOutput(true);
    // opus_decoder_->ResetState();
    // audio_decode_queue_.clear();
    SetDecodeSampleRate(16000);
    const char *data = sound.data();
    size_t size = sound.size();

    for (const char *p = data; p < data + size;)
    {
        auto p3 = (BinaryProtocol3 *)p;
        p += sizeof(BinaryProtocol3);

        auto payload_size = ntohs(p3->payload_size);
        std::vector<uint8_t> opus;
        opus.resize(payload_size);
        memcpy(opus.data(), p3->payload, payload_size);
        p += payload_size;

        std::lock_guard<std::mutex> lock(mutex_);
        audio_decode_queue_.emplace_back(std::move(opus));
    }
}

void Application::ToggleChatState()
{
    if (device_state_ == kDeviceStateActivating)
    {
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (!protocol_)
    {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle)
    {
        Schedule([this]()
                 {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                return;
            }

            keep_listening_ = true;
            protocol_->SendStartListening(kListeningModeAutoStop);
            SetDeviceState(kDeviceStateListening); });
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        Schedule([this]()
                 { AbortSpeaking(kAbortReasonWakeWordDetected); }); // kAbortReasonNone
    }
    else if (device_state_ == kDeviceStateListening)
    {

        Schedule([this]()
                 { protocol_->CloseAudioChannel(); });
    }
}

void Application::StartListening()
{
    if (device_state_ == kDeviceStateActivating)
    {
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (!protocol_)
    {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    keep_listening_ = false;
    if (device_state_ == kDeviceStateIdle)
    {
        Schedule([this]()
                 {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }
            protocol_->SendStartListening(kListeningModeManualStop);
            SetDeviceState(kDeviceStateListening); });
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        Schedule([this]()
                 {
            AbortSpeaking(kAbortReasonNone);
            protocol_->SendStartListening(kListeningModeManualStop);
            SetDeviceState(kDeviceStateListening); });
    }
}

void Application::StopListening()
{
    Schedule([this]()
             {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        } });
}

void Application::Start()
{
    // if(flag_sound!=1)
    // {
    auto &board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    /* Setup the audio codec */
    auto codec = board.GetAudioCodec();
    opus_decode_sample_rate_ = codec->output_sample_rate();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(opus_decode_sample_rate_, 1);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    // For ML307 boards, we use complexity 5 to save bandwidth
    // For other boards, we use complexity 3 to save CPU
    if (board.GetBoardType() == "ml307")
    {
        ESP_LOGI(TAG, "ML307 board detected, setting opus encoder complexity to 5");
        opus_encoder_->SetComplexity(5);
    }
    else
    {
        ESP_LOGI(TAG, "WiFi board detected, setting opus encoder complexity to 3");
        opus_encoder_->SetComplexity(3);
    }

    if (codec->input_sample_rate() != 16000)
    {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }
    codec->OnInputReady([this, codec]()
                        {
        BaseType_t higher_priority_task_woken = pdFALSE;
        xEventGroupSetBitsFromISR(event_group_, AUDIO_INPUT_READY_EVENT, &higher_priority_task_woken);
        return higher_priority_task_woken == pdTRUE; });
    codec->OnOutputReady([this]()
                         {
        BaseType_t higher_priority_task_woken = pdFALSE;
        xEventGroupSetBitsFromISR(event_group_, AUDIO_OUTPUT_READY_EVENT, &higher_priority_task_woken);
        return higher_priority_task_woken == pdTRUE; });
    codec->Start();

    /* Start the main loop */
    xTaskCreate([](void *arg)
                {
        Application* app = (Application*)arg;
        app->MainLoop();
        vTaskDelete(NULL); }, "main_loop", 4096 * 2, this, 4, nullptr);

    // // 创建定时任务
    // xTaskCreate([](void *arg)
    //             {
    //     Application* app = (Application*)arg;
    //     app->applicant_task();
    //     vTaskDelete(NULL); }, "applicant_task", 4096 * 2, this, 4, nullptr);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);
#ifdef CONFIG_CONNECTION_TYPE_WEBSOCKET
    protocol_ = std::make_unique<WebsocketProtocol>();
#else
    protocol_ = std::make_unique<MqttProtocol>();
#endif
    protocol_->OnNetworkError([this](const std::string &message)
                              {
        SetDeviceState(kDeviceStateIdle);
        Alert(Lang::Strings::ERROR, message.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION); });
    protocol_->OnIncomingAudio([this](std::vector<uint8_t> &&data)
                               {
        std::lock_guard<std::mutex> lock(mutex_);
        if (device_state_ == kDeviceStateSpeaking) {
            audio_decode_queue_.emplace_back(std::move(data));
        } });
    protocol_->OnAudioChannelOpened([this, codec, &board]()
                                    {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {

            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
        SetDecodeSampleRate(protocol_->server_sample_rate());
        auto& thing_manager = iot::ThingManager::GetInstance();
        protocol_->SendIotDescriptors(thing_manager.GetDescriptorsJson());
        std::string states;
        if (thing_manager.GetStatesJson(states, false)) {
            protocol_->SendIotStates(states);
        } });
    protocol_->OnAudioChannelClosed([this, &board]()
                                    {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        }); });
    protocol_->OnIncomingJson([this, display](const cJSON *root)
                              {
        // if (yt_command_flag == Bluetooth_mode ||yt_command_flag == Wake_word_ended||yt_command_flag ==Wake_word_pattern
        // || yt_command_flag ==Bluetooth_disconnected || yt_command_flag ==Bluetooth_connected  || yt_command_flag ==Last_song || yt_command_flag ==Next_song
        // ||yt_command_flag ==Maximum_volume ||yt_command_flag ==Minimum_volume ||yt_command_flag ==Bluetooth_pause ||yt_command_flag ==Bluetooth_playing ) { //test
        //     ESP_LOGI(TAG, "Ignore JSON in Bluetooth mode");
        //     return;  // 蓝牙模式下不处理任何协议指令
        // }
        // Only ignore JSON when explicitly in Bluetooth_mode, not when flag is just cleared (0)
        if (yt_command_flag == Bluetooth_mode) { //test
                ESP_LOGI(TAG, "Ignore JSON in Bluetooth mode");
                return;  // 蓝牙模式下不处理任何协议指令
        }
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                        if (device_state_ == kDeviceStateSpeaking) {
                            background_task_->WaitForCompletion();
                            if (keep_listening_) {
                                protocol_->SendStartListening(kListeningModeAutoStop);
                                SetDeviceState(kDeviceStateListening);
                            } else {
                                SetDeviceState(kDeviceStateIdle);
                            }
                        }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (text != NULL) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (text != NULL) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (emotion != NULL) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    // SendEmotionByString(emotion_str.c_str());
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "iot") == 0) {
            auto commands = cJSON_GetObjectItem(root, "commands");
            if (commands != NULL) {
                auto& thing_manager = iot::ThingManager::GetInstance();
                for (int i = 0; i < cJSON_GetArraySize(commands); ++i) {
                    auto command = cJSON_GetArrayItem(commands, i);
                    thing_manager.Invoke(command);
                }
            }
        } });
    protocol_->Start();
    // Check for new firmware version or get the MQTT broker address
    ota_.SetCheckVersionUrl(CONFIG_OTA_VERSION_URL);
    ota_.SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    ota_.SetHeader("Client-Id", board.GetUuid());
    ota_.SetHeader("Accept-Language", Lang::CODE);
    auto app_desc = esp_app_get_description();
    ota_.SetHeader("User-Agent", std::string(BOARD_NAME "/") + app_desc->version);

    xTaskCreate([](void* arg) {
     Application* app = (Application*)arg;
    app->CheckNewVersion();
      vTaskDelete(NULL);
    }, "check_new_version", 4096 * 2, this, 3, nullptr);   //2

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_.Initialize(codec->input_channels(), codec->input_reference());
    audio_processor_.OnOutput([this](std::vector<int16_t> &&data)
                              { background_task_->Schedule([this, data = std::move(data)]() mutable
                                                           { opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t> &&opus)
                                                                                   { Schedule([this, opus = std::move(opus)]()
                                                                                              { protocol_->SendAudio(opus); }); }); }); });
    audio_processor_.OnVadStateChange([this](bool speaking)
                                      {
        if (device_state_ == kDeviceStateListening) {
            Schedule([this, speaking]() {
                if (speaking) {
                    voice_detected_ = true;
                } else {
                    voice_detected_ = false;
                }
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            });
        } });
#endif

#if CONFIG_USE_WAKE_WORD_DETECT
    wake_word_detect_.Initialize(codec->input_channels(), codec->input_reference());
    wake_word_detect_.OnWakeWordDetected([this](const std::string &wake_word)
                                         { Schedule([this, &wake_word]()
                                                    {
            if (device_state_ == kDeviceStateIdle) {
                SetDeviceState(kDeviceStateConnecting);
                wake_word_detect_.EncodeWakeWordData();

                if (!protocol_->OpenAudioChannel()) {
                    wake_word_detect_.StartDetection();
                    return;
                }

                std::vector<uint8_t> opus;
                // Encode and send the wake word data to the server
                while (wake_word_detect_.GetWakeWordOpus(opus)) {
                    protocol_->SendAudio(opus);
                }
                // Set the chat state to wake word detected
                protocol_->SendWakeWordDetected(wake_word);
                ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
                keep_listening_ = true;
                SetDeviceState(kDeviceStateIdle);
            } else if (device_state_ == kDeviceStateSpeaking) {
                AbortSpeaking(kAbortReasonWakeWordDetected);
            } else if (device_state_ == kDeviceStateActivating) {
                SetDeviceState(kDeviceStateIdle);
            } }); });
    wake_word_detect_.StartDetection();
#endif

    SetDeviceState(kDeviceStateIdle);  //xkDeviceStateIdle
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // 启动完成后默认开启幻灯片，确保有 GIF 显示
    background_task_->Schedule([this]() {
        vTaskDelay(pdMS_TO_TICKS(3000));  // 等待系统稳定
        if (device_state_ == kDeviceStateIdle && !IsSlideShowRunning()) {
            ESP_LOGI(TAG, "System startup complete, starting SlideShow");
            SlideShow();
        }
    });
    // }
}

void Application::OnClockTimer()
{
    clock_ticks_++;

    // Print the debug info every 10 seconds
    if (clock_ticks_ % 10 == 0)
    {
        // SystemInfo::PrintRealTimeStats(pdMS_TO_TICKS(1000));
        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "Free internal: %u minimal internal: %u", free_sram, min_free_sram);

        // If we have synchronized server time, set the status to clock "HH:MM" if the device is idle
        if (ota_.HasServerTime())
        {
            if (device_state_ == kDeviceStateIdle)
            {
                Schedule([this]()
                         {
                    // Set status to clock "HH:MM"
                    time_t now = time(NULL);
                    char time_str[64];
                    strftime(time_str, sizeof(time_str), "%H:%M  ", localtime(&now));
                    Board::GetInstance().GetDisplay()->SetStatus(time_str); });
            }
        }
    }

    // GIF测试：每45秒切换一次显示/隐藏状态（避开时钟更新时间）
    if (device_state_ == kDeviceStateIdle && !slideshow_running_ && clock_ticks_ > 15) // 启动15秒后开始（幻灯片运行时跳过自动切换）
    {
        // 使用45秒周期，避开10秒的时钟更新周期
        if (clock_ticks_ % 45 == 0) // 每45秒
        {
            // 延迟2秒执行，避免与时钟更新冲突
            background_task_->Schedule([this]() {
                vTaskDelay(pdMS_TO_TICKS(2000));  // 延迟2秒
                if (device_state_ == kDeviceStateIdle) { // 再次检查状态
                    ESP_LOGI(TAG, "Auto showing URL GIF (delayed)");
                    SlideShow();
                }
            });
        }
    }
}

void Application::Schedule(std::function<void()> callback)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT);
}

// The Main Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainLoop()
{
    while (true)
    {
        // if(flag_sound!=1)
        // {
        auto bits = xEventGroupWaitBits(event_group_,
                                        SCHEDULE_EVENT | AUDIO_INPUT_READY_EVENT | AUDIO_OUTPUT_READY_EVENT,
                                        pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & AUDIO_INPUT_READY_EVENT)
        {
            InputAudio();
        }
        if (bits & AUDIO_OUTPUT_READY_EVENT)
        {
            OutputAudio();
        }
        if (bits & SCHEDULE_EVENT)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            std::list<std::function<void()>> tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto &task : tasks)
            {
                task();
            }
        }
        // }
    }
}

void Application::ResetDecoder()
{
    std::lock_guard<std::mutex> lock(mutex_);
    opus_decoder_->ResetState();
    audio_decode_queue_.clear();
}
void Application::Clearaudio()
{
    std::lock_guard<std::mutex> lock(mutex_);
    opus_decoder_->ResetState();
    audio_decode_queue_.clear();
    // opus_encoder_->ResetState(); //加
    last_output_time_ = std::chrono::steady_clock::now();
}
void Application::OutputAudio()
{
    auto now = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int max_silence_seconds = 10;
    std::unique_lock<std::mutex> lock(mutex_);
    if (audio_decode_queue_.empty())
    {
        // lock.unlock();
        // Disable the output if there is no audio data for a long time
        if (device_state_ == kDeviceStateIdle)
        {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_output_time_).count();
            if (duration > max_silence_seconds)
            {
                codec->EnableOutput(false);
            }
        }
        return;
    }

    if (device_state_ == kDeviceStateListening)
    {
        audio_decode_queue_.clear();
        return;
    }

    last_output_time_ = now;
    auto opus = std::move(audio_decode_queue_.front());
    audio_decode_queue_.pop_front();
    lock.unlock();

    background_task_->Schedule([this, codec, opus = std::move(opus)]() mutable
                               {
        if (aborted_) {
            return;
        }

        std::vector<int16_t> pcm;
        if (!opus_decoder_->Decode(std::move(opus), pcm)) {
            return;
        }

        // Resample if the sample rate is different
        if (opus_decode_sample_rate_ != codec->output_sample_rate()) {
            int target_size = output_resampler_.GetOutputSamples(pcm.size());
            std::vector<int16_t> resampled(target_size);
            output_resampler_.Process(pcm.data(), pcm.size(), resampled.data());
            pcm = std::move(resampled);
        }

        codec->OutputData(pcm); });
}

void Application::InputAudio()
{
    auto codec = Board::GetInstance().GetAudioCodec();
    std::vector<int16_t> data;
    if (!codec->InputData(data))
    {
        return;
    }

    if (codec->input_sample_rate() != 16000)
    {
        if (codec->input_channels() == 2)
        {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2)
            {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2)
            {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        }
        else
        {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    }

#if CONFIG_USE_WAKE_WORD_DETECT
    if (wake_word_detect_.IsDetectionRunning())
    {
        wake_word_detect_.Feed(data);
    }
#endif
#if CONFIG_USE_AUDIO_PROCESSOR
    if (audio_processor_.IsRunning())
    {
        audio_processor_.Input(data);
    }
#else
    if (device_state_ == kDeviceStateListening)
    {
        background_task_->Schedule([this, data = std::move(data)]() mutable
                                   { opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t> &&opus)
                                                           { Schedule([this, opus = std::move(opus)]()
                                                                      { protocol_->SendAudio(opus); }); }); });
    }
#endif
}

void Application::AbortSpeaking(AbortReason reason)
{
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true; // 开
    protocol_->SendAbortSpeaking(reason);
}
uint8_t Listen_state = 1;

void Application::SetDeviceState(DeviceState state)
{
    if (device_state_ == state)
    {
        return;
    }
    // std::lock_guard<std::mutex> lock(mutex_);
    // opus_decoder_->ResetState();  // 重置解码器
    // audio_decode_queue_.clear();  // 清空队列
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);
    // The state is changed, wait for all background tasks to finish
    background_task_->WaitForCompletion();
    auto &board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    auto display = board.GetDisplay();
    // auto led = board.GetLed();
    // led->OnStateChanged();
    switch (state)
    {
    case kDeviceStateUnknown:
    case kDeviceStateIdle:
        gpio_set_level(GPIO_NUM_11, 1);
        // if(flag_sound)
        // {
        //     gpio_set_level(GPIO_NUM_11, 0);
        //     // flag_sound=0;
        // }
        // gpio_set_level(GPIO_NUM_11, 0);
        // PlaySound(Lang::Sounds::P3_SUCCESS);
        // vTaskDelay(pdMS_TO_TICKS(400));
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");

        // SendEmotionByString("NEUTRAL");
#if CONFIG_USE_AUDIO_PROCESSOR
        audio_processor_.Stop();
#endif
#if CONFIG_USE_WAKE_WORD_DETECT
        wake_word_detect_.StartDetection();
#endif
        break;
    case kDeviceStateConnecting:
        // gpio_set_level(GPIO_NUM_11, 0);
        // PlaySound(Lang::Sounds::P3_SUCCESS);
        display->SetStatus(Lang::Strings::CONNECTING);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");

        // codec->EnableOutput(true);
        // gpio_set_level(GPIO_NUM_11, 0);
        // vTaskDelay(pdMS_TO_TICKS(120));
        // // PlaySound(Lang::Sounds::P3_SUCCESS);
        // PlaySound(Lang::Sounds::P3_SUCCESS); // 播放提示音
        break;
    case kDeviceStateListening:
        // SendEmotionByString("happy");
        gpio_set_level(GPIO_NUM_11, 1);
        display->SetStatus(Lang::Strings::LISTENING);
        display->SetEmotion("neutral");
        ResetDecoder();
        opus_encoder_->ResetState();
        // UpdateIotStates();
        // codec->EnableOutput(true);

        // PlaySound(Lang::Sounds::P3_SUCCESS);

        // // 延迟等待提示音播放完成（根据音频长度调整，例如 500ms）
        // vTaskDelay(pdMS_TO_TICKS(500));

        // // 关闭音频输出（但保持功放开启，以便后续语音输入）
        // codec->EnableOutput(false);
#if CONFIG_USE_AUDIO_PROCESSOR
        audio_processor_.Start();
#endif
#if CONFIG_USE_WAKE_WORD_DETECT
        wake_word_detect_.StopDetection();
#endif

        if (previous_state == kDeviceStateSpeaking)
        {
            // PlaySound(Lang::Sounds::P3_SUCCESS);  // 播放提示音
            // FIXME: Wait for the speaker to empty the buffer
            vTaskDelay(pdMS_TO_TICKS(120)); // 120
        }
        break;

    case kDeviceStateSpeaking:
        // vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(GPIO_NUM_11, 0);
        display->SetStatus(Lang::Strings::SPEAKING);
        ResetDecoder();
        codec->EnableOutput(true);
#if CONFIG_USE_AUDIO_PROCESSOR
        audio_processor_.Stop();
#endif
#if CONFIG_USE_WAKE_WORD_DETECT
        wake_word_detect_.StartDetection();
#endif
        break;

    default:
        // Do nothing
        break;
    }
}

void Application::SetDecodeSampleRate(int sample_rate)
{
    if (opus_decode_sample_rate_ == sample_rate)
    {
        return;
    }

    opus_decode_sample_rate_ = sample_rate;
    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(opus_decode_sample_rate_, 1);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decode_sample_rate_ != codec->output_sample_rate())
    {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decode_sample_rate_, codec->output_sample_rate());
        output_resampler_.Configure(opus_decode_sample_rate_, codec->output_sample_rate());
    }
}

void Application::UpdateIotStates()
{
    auto &thing_manager = iot::ThingManager::GetInstance();
    std::string states;
    if (thing_manager.GetStatesJson(states, true))
    {
        protocol_->SendIotStates(states);
    }
}

void Application::Reboot()
{
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void Application::WakeWordInvoke(const std::string &wake_word)
{
    if (device_state_ == kDeviceStateIdle)
    {
        ToggleChatState();
        Schedule([this, wake_word]()
                 {
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word);
            } });
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        Schedule([this]()
                 { AbortSpeaking(kAbortReasonNone); });
    }
    else if (device_state_ == kDeviceStateListening)
    {
        Schedule([this]()
                 {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            } });
    }
}
void Application::WakeWordInvoke1()
{
    if (device_state_ == kDeviceStateIdle)
    {
        ToggleChatState();
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        Schedule([this]()
                 { AbortSpeaking(kAbortReasonNone); });
    }
    else if (device_state_ == kDeviceStateListening)
    {
        Schedule([this]()
                 {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            } });
    }
}
bool Application::CanEnterSleepMode()
{
    if (device_state_ != kDeviceStateIdle)
    {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened())
    {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::PrintMemoryInfo()
{
    ESP_LOGI(TAG, "=== ESP32 Memory Information ===");

    // Internal RAM
    uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    uint32_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    ESP_LOGI(TAG, "Internal RAM - Total: %lu, Free: %lu, Min: %lu, Used: %lu",
             internal_total, internal_free, internal_min, internal_total - internal_free);

    // SPIRAM (if available)
    uint32_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t spiram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (spiram_total > 0) {
        ESP_LOGI(TAG, "SPIRAM - Total: %lu, Free: %lu, Used: %lu",
                 spiram_total, spiram_free, spiram_total - spiram_free);
    } else {
        ESP_LOGI(TAG, "SPIRAM - Not available");
    }

    // DMA capable memory
    uint32_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    uint32_t dma_total = heap_caps_get_total_size(MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "DMA Memory - Total: %lu, Free: %lu, Used: %lu",
             dma_total, dma_free, dma_total - dma_free);

    // Overall heap
    uint32_t total_free = esp_get_free_heap_size();
    uint32_t total_min = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "Total Heap - Free: %lu, Min: %lu", total_free, total_min);

    // Largest free block
    uint32_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Largest free block (Internal): %lu bytes", largest_block);

    ESP_LOGI(TAG, "================================");
}

void Application::ShowGif(const uint8_t* gif_data, size_t gif_size, int x, int y)
{
    // Print detailed memory information (only once for debugging)
    static bool memory_info_printed = false;
    if (!memory_info_printed) {
        PrintMemoryInfo();
        memory_info_printed = true;
    }

    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        ESP_LOGI(TAG, "Application: Showing GIF animation");
        display->ShowGif(gif_data, gif_size, x, y);

        // Check memory after GIF creation
        ESP_LOGI(TAG, "Application: Memory after GIF - Free: %lu", (unsigned long)esp_get_free_heap_size());
    } else {
        ESP_LOGE(TAG, "Display not available for GIF");
    }
}

void Application::ShowGifFromUrl(const char* url, int x, int y)
{
    if (url == nullptr || strlen(url) == 0) {
        ESP_LOGE(TAG, "Invalid URL provided for GIF display");
        return;
    }

    ESP_LOGI(TAG, "Application: Starting GIF download from URL: %s", url);

    // Print memory information
    PrintMemoryInfo();

    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        ESP_LOGI(TAG, "Application: Downloading and showing GIF from URL");
        display->ShowGifFromUrl(url, x, y);

        // Check memory after download attempt
        ESP_LOGI(TAG, "Application: Memory after GIF download - Free: %lu", (unsigned long)esp_get_free_heap_size());
    } else {
        ESP_LOGE(TAG, "Display not available for GIF URL download");
    }
}

void Application::HideGif()
{
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        ESP_LOGI(TAG, "Application: Hiding GIF animation");
        display->HideGif();
    }
}

bool Application::IsGifPlaying() const
{
    auto display = Board::GetInstance().GetDisplay();
    return display ? display->IsGifPlaying() : false;
}

// Download a GIF into PSRAM buffer (no display). Caller must free *out_buf with heap_caps_free.
static bool DownloadGifToPsram(const char* url, uint8_t** out_buf, size_t* out_len) {
    if (!url || !out_buf || !out_len) return false;
    *out_buf = nullptr;
    *out_len = 0;

    const int kMaxRetries = 2;           // restart whole download up to 2 times
    const size_t kDefaultCap = 512 * 1024;
    const size_t kMaxSize   = 10 * 1024 * 1024; // 10MB cap
    const int kRxChunk = 16384;                 // read chunk goes directly into PSRAM dest buffer

    for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
        esp_http_client_config_t cfg = {};
        cfg.url = url;
        cfg.timeout_ms = 60000;          // increase socket timeout
        // Keep esp_http_client internal RX buffer small to save SRAM; data goes to PSRAM via read()
        cfg.buffer_size = 2048;          // small header/parse buffer in SRAM
        cfg.buffer_size_tx = 2048;
        cfg.keep_alive_enable = false;   // avoid keep-alive surprises on some servers
        if (strncmp(url, "https://", 8) == 0) {
            cfg.use_global_ca_store = true;
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
        }

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGE(TAG, "Download init failed (attempt %d)", attempt + 1);
            continue;
        }

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s (attempt %d)", esp_err_to_name(err), attempt + 1);
            esp_http_client_cleanup(client);
            continue;
        }

        // Fetch headers and get content length if provided
        esp_http_client_fetch_headers(client);
        int content_length = esp_http_client_get_content_length(client);
        size_t cap = kDefaultCap;
        if (content_length > 0) {
            cap = (size_t)content_length;
            if (cap > kMaxSize) {
                ESP_LOGE(TAG, "File too large: %d bytes", content_length);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return false;
            }
        }

        uint8_t* buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            ESP_LOGE(TAG, "PSRAM alloc failed: %u bytes", (unsigned)cap);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }

        size_t pos = 0;
        size_t last_progress = 0;
        size_t last_yield_bytes = 0;
        const size_t kYieldEvery = 64 * 1024;

        bool ok = true;
        while (true) {
            int r = esp_http_client_read(client, (char*)buf + pos, kRxChunk);
            if (r < 0) {
                ESP_LOGE(TAG, "HTTP read error: %d (attempt %d)", r, attempt + 1);
                ok = false;
                break;
            }
            if (r == 0) break; // done
            pos += (size_t)r;

            // Grow if needed for unknown content length
            if (pos > cap) {
                size_t new_cap = cap * 2;
                if (new_cap < pos) new_cap = pos;
                if (new_cap > kMaxSize) {
                    ESP_LOGE(TAG, "Download exceeds max cap (%u > %u)", (unsigned)new_cap, (unsigned)kMaxSize);
                    ok = false;
                    break;
                }
                uint8_t* nb = (uint8_t*)heap_caps_realloc(buf, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!nb) {
                    ESP_LOGE(TAG, "PSRAM realloc failed: %u bytes", (unsigned)new_cap);
                    ok = false;
                    break;
                }
                buf = nb;
                cap = new_cap;
            }

            // Progress (if content length known)
            if (content_length > 0) {
                size_t prog = (pos * 100) / (size_t)content_length;
                if (prog >= last_progress + 20) {
                    ESP_LOGI(TAG, "Download progress: %u%% (%u/%u bytes)", (unsigned)prog, (unsigned)pos, (unsigned)content_length);
                    last_progress = prog;
                }
            }
            // Yield periodically to feed WDT
            if (pos - last_yield_bytes >= kYieldEvery) {
                vTaskDelay(1);
                last_yield_bytes = pos;
            }
        }

        int status = esp_http_client_get_status_code(client);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (ok && (status == 200 || status == 206) && pos >= 6 && (memcmp(buf, "GIF87a", 6) == 0 || memcmp(buf, "GIF89a", 6) == 0)) {
            *out_buf = buf;
            *out_len = pos;
            ESP_LOGI(TAG, "Downloaded GIF: %u bytes", (unsigned)pos);
            return true;
        }

        // failure path
        ESP_LOGE(TAG, "Download failed: status=%d, size=%u (attempt %d)", status, (unsigned)pos, attempt + 1);
        heap_caps_free(buf);
        if (attempt < kMaxRetries) {
            vTaskDelay(pdMS_TO_TICKS(500 * (attempt + 1))); // backoff before retry
        }
    }

    return false;
}

bool Application::IsSlideShowRunning() const
{
    return slideshow_running_.load();
}

void Application::SlideShowNext()
{
    if (IsSlideShowRunning()) {
        slideshow_skip_.store(1);
        ESP_LOGI(TAG, "SlideShowNext requested by gesture");
    }
}

void Application::SlideShowPrev()
{
    if (IsSlideShowRunning()) {
        slideshow_skip_.store(-1);
        ESP_LOGI(TAG, "SlideShowPrev requested by gesture");
    }
}

extern "C" bool app_is_slideshow_running(void) {
    return Application::GetInstance().IsSlideShowRunning();
}
extern "C" void app_slideshow_next(void) {
    Application::GetInstance().SlideShowNext();
}
extern "C" void app_slideshow_prev(void) {
    Application::GetInstance().SlideShowPrev();
}


void Application::SlideShow()
{
    // Prevent concurrent slideshows
    if (slideshow_running_) {
        ESP_LOGW(TAG, "SlideShow already running, ignore request");
        return;
    }
    stop_slideshow_ = false;
    slideshow_running_ = true;

    background_task_->Schedule([this]() {
        // List of GIF URLs to preload, then display sequentially
        static const char* kGifUrls[] = {
            //"http://122.51.57.185:18080/test1.gif",
            //"http://122.51.57.185:18080/test2.gif",
            //"http://122.51.57.185:18080/test3.gif",
            "http://122.51.57.185:18080/huahua-1.gif",
            //"http://122.51.57.185:18080/412_Normal.gif",
            //"http://122.51.57.185:18080/412_think.gif",
            //"http://122.51.57.185:18080/412_angry.gif",
            "http://122.51.57.185:18080/412_cheer.gif",
            "http://122.51.57.185:18080/412_sadly.gif"
        };
        constexpr int kCount = sizeof(kGifUrls) / sizeof(kGifUrls[0]);

        ESP_LOGI(TAG, "SlideShow started (preload + loop) (%d items)", kCount);

        // Ensure no decoding during download phase
        if (auto display = Board::GetInstance().GetDisplay()) {
            display->HideGif();
        }

        struct PreGif { uint8_t* data; size_t size; const char* url; };
        PreGif items[kCount];
        for (int i = 0; i < kCount; ++i) {
            items[i].data = nullptr;
            items[i].size = 0;
            items[i].url = kGifUrls[i];
        }

        int loaded = 0;
        // Download all GIFs first (no decoding)
        for (int i = 0; i < kCount && !stop_slideshow_; ++i) {
            if (device_state_ != kDeviceStateIdle) {
                ESP_LOGW(TAG, "Device not idle, abort SlideShow preload");
                stop_slideshow_ = true;
                break;
            }
            ESP_LOGI(TAG, "Pre-download %d/%d: %s", i + 1, kCount, kGifUrls[i]);
            uint8_t* buf = nullptr; size_t len = 0;
            if (DownloadGifToPsram(kGifUrls[i], &buf, &len)) {
                items[loaded].data = buf;
                items[loaded].size = len;
                items[loaded].url  = kGifUrls[i];
                ++loaded;
            } else {
                if (buf) heap_caps_free(buf);
                ESP_LOGE(TAG, "Pre-download failed: %s", kGifUrls[i]);
            }
            vTaskDelay(1);
        }

        if (loaded == 0 || stop_slideshow_) {
            ESP_LOGW(TAG, "No GIFs preloaded or slideshow stopped during preload");
            if (auto display = Board::GetInstance().GetDisplay()) display->HideGif();
            for (int i = 0; i < kCount; ++i) if (items[i].data) heap_caps_free(items[i].data);
            stop_slideshow_ = false;
            slideshow_running_ = false;
            ESP_LOGI(TAG, "SlideShow finished");
            return;
        }

        ESP_LOGI(TAG, "Pre-download completed: %d/%d", loaded, kCount);

        // Display phase (no downloading)
        int index = 0;
        while (!stop_slideshow_) {
            if (device_state_ != kDeviceStateIdle) {
                ESP_LOGW(TAG, "Device state changed, abort SlideShow");
                stop_slideshow_ = true;
                break;
            }
            // Normalize index
            if (index < 0) index = (loaded - 1);
            if (index >= loaded) index = 0;

            ESP_LOGI(TAG, "SlideShow showing %d/%d: %s", index + 1, loaded, items[index].url);
            if (auto display = Board::GetInstance().GetDisplay()) {
                display->ShowGif(items[index].data, items[index].size, 0, 0);
            }
            // wait for user swipe to change item; do not auto-advance when GIF finishes
            while (!stop_slideshow_) {
                if (device_state_ != kDeviceStateIdle) {
                    ESP_LOGW(TAG, "Device state changed, abort SlideShow");
                    stop_slideshow_ = true;
                    break;
                }
                int skip = slideshow_skip_.exchange(0);
                if (skip != 0) {
                    index += skip; // -1 prev, +1 next (from gesture)
                    goto next_item;
                }
                // Keep current GIF looping until user swipes to change item
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            // no automatic index change here; wait strictly for gestures
        next_item:
            continue;
        }

        // Clean up display and free preloaded buffers
        if (auto display = Board::GetInstance().GetDisplay())
            display->HideGif();
        for (int i = 0; i < loaded; ++i) {
            if (items[i].data)
                heap_caps_free(items[i].data);
        }
        stop_slideshow_ = false;
        slideshow_running_ = false;
        ESP_LOGI(TAG, "SlideShow finished");
    });
}

void Application::StopSlideShow()
{
    if (!slideshow_running_) return;
    stop_slideshow_ = true;
    ESP_LOGI(TAG, "SlideShow stop requested");
}
