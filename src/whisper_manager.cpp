#include "whisper_manager.h"
#include <whisper.h>
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include <iostream>
#include <filesystem>
#include <cstring>
#include <cmath>
#include <regex>
#include <algorithm>
#include "gemini_client.h"
#include "config.h"
#include "ui.h"
#include "world.h"
#include "tool_manager.h"
#include <gtk/gtk.h>

namespace fs = std::filesystem;

namespace {
// Audio capture constants
const int SAMPLE_RATE = 16000;
const int AUDIO_BUFFER_MS = 10000; // Keep 10 seconds of audio buffer

class AudioCapture {
public:
    AudioCapture() : m_dev_id_in(0), m_running(false), m_audio_pos(0), m_audio_len(0) {}
    ~AudioCapture() { close(); }

    bool init() {
        SDL_AudioSpec capture_spec_requested;
        SDL_AudioSpec capture_spec_obtained;

        SDL_zero(capture_spec_requested);
        SDL_zero(capture_spec_obtained);

        capture_spec_requested.freq = SAMPLE_RATE;
        capture_spec_requested.format = AUDIO_F32;
        capture_spec_requested.channels = 1;
        capture_spec_requested.samples = 2048;
        capture_spec_requested.callback = [](void* userdata, uint8_t* stream, int len) {
            ((AudioCapture*)userdata)->callback(stream, len);
        };
        capture_spec_requested.userdata = this;

        // Try to open default capture device
        m_dev_id_in = SDL_OpenAudioDevice(nullptr, SDL_TRUE, &capture_spec_requested, &capture_spec_obtained, 0);
        if (!m_dev_id_in) {
            std::cerr << "Whisper: failed to open default audio device: " << SDL_GetError() << std::endl;
            return false;
        }

        m_audio.resize((SAMPLE_RATE * AUDIO_BUFFER_MS) / 1000, 0.0f);
        return true;
    }

    void close() {
        if (m_dev_id_in) {
            SDL_CloseAudioDevice(m_dev_id_in);
            m_dev_id_in = 0;
        }
    }

    void resume() { if (m_dev_id_in) { SDL_PauseAudioDevice(m_dev_id_in, 0); m_running = true; } }
    void pause() { if (m_dev_id_in) { SDL_PauseAudioDevice(m_dev_id_in, 1); m_running = false; } }

    void callback(uint8_t* stream, int len) {
        if (!m_running) return;
        size_t n_samples = len / sizeof(float);
        std::lock_guard<std::mutex> lock(m_mutex);
        
        if (m_audio_pos + n_samples > m_audio.size()) {
            size_t n0 = m_audio.size() - m_audio_pos;
            std::memcpy(&m_audio[m_audio_pos], stream, n0 * sizeof(float));
            std::memcpy(&m_audio[0], (float*)stream + n0, (n_samples - n0) * sizeof(float));
        } else {
            std::memcpy(&m_audio[m_audio_pos], stream, n_samples * sizeof(float));
        }
        
        m_audio_pos = (m_audio_pos + n_samples) % m_audio.size();
        m_audio_len = std::min(m_audio_len + n_samples, m_audio.size());
    }

    void get(int ms, std::vector<float>& result) {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t n_samples = (SAMPLE_RATE * ms) / 1000;
        if (n_samples > m_audio_len) n_samples = m_audio_len;
        
        result.assign(n_samples, 0.0f);
        int s0 = (int)m_audio_pos - (int)n_samples;
        if (s0 < 0) s0 += (int)m_audio.size();
        
        if (s0 + n_samples > m_audio.size()) {
            size_t n0 = m_audio.size() - s0;
            std::memcpy(result.data(), &m_audio[s0], n0 * sizeof(float));
            std::memcpy(result.data() + n0, &m_audio[0], (n_samples - n0) * sizeof(float));
        } else {
            std::memcpy(result.data(), &m_audio[s0], n_samples * sizeof(float));
        }
    }

    float get_peak() {
        std::lock_guard<std::mutex> lock(m_mutex);
        float peak = 0.0f;
        size_t n = (SAMPLE_RATE * 50) / 1000; // Look at last 50ms for performance
        if (n > m_audio_len) n = m_audio_len;
        for (size_t i = 0; i < n; ++i) {
            int idx = (int)m_audio_pos - 1 - (int)i;
            if (idx < 0) idx += (int)m_audio.size();
            peak = std::max(peak, std::abs(m_audio[idx]));
        }
        return peak;
    }

private:
    SDL_AudioDeviceID m_dev_id_in;
    std::atomic<bool> m_running;
    std::mutex m_mutex;
    std::vector<float> m_audio;
    size_t m_audio_pos;
    size_t m_audio_len;
};

std::mutex g_stateMutex;
WhisperVisualizerState g_state;
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<bool> g_stopRequested{false};

void worker_loop() {
    AudioCapture capture;
    if (!capture.init()) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state.status = "Err: Audio device fail";
        g_state.running = false;
        return;
    }
    capture.resume();

    // Model search paths
    std::vector<std::string> modelPaths = {
        "third_party/whisper.cpp/models/ggml-tiny.en-q5_1.bin",
        "Assets/whisper-tiny.en.bin",
        "third_party/whisper.cpp/models/for-tests-ggml-tiny.en.bin",
        "models/ggml-tiny.en.bin"
    };
    
    std::string modelFile;
    for(const auto& p : modelPaths) {
        if(fs::exists(p)) { modelFile = p; break; }
    }

    if(modelFile.empty()) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state.status = "Err: Model not found";
        g_state.running = false;
        return;
    }

    whisper_context_params cparams = whisper_context_default_params();
    struct whisper_context* ctx = whisper_init_from_file_with_params(modelFile.c_str(), cparams);
    if (!ctx) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state.status = "Err: Model load failed";
        g_state.running = false;
        return;
    }

    std::vector<float> pcmf32;
    
    // Silence detection state
    bool isSpeaking = false;
    auto lastSpeechTime = std::chrono::steady_clock::now();
    auto startSpeechTime = std::chrono::steady_clock::now();
    const float SILENCE_THRESHOLD = 0.02f; // Sensitivity adjustment
    const int SILENCE_MS_TRIGGER = 1200;    // 1.2s of silence triggers processing

    while (!g_stopRequested.load(std::memory_order_relaxed)) {
        float peak = capture.get_peak();
        auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state.peakLevel = peak;
            for (size_t i = 0; i < g_state.bars.size(); ++i) {
                float target = peak * (0.5f + 0.5f * std::sin(i * 0.5f + peak * 10.0f));
                g_state.bars[i] = g_state.bars[i] * 0.7f + target * 0.3f;
            }
            g_state.status = isSpeaking ? "Listening..." : "Waiting for voice...";
            g_state.running = true;
            g_state.lastUpdateTime = std::chrono::duration<double>(now.time_since_epoch()).count();
        }

        if (peak > SILENCE_THRESHOLD) {
            if (!isSpeaking) {
                isSpeaking = true;
                startSpeechTime = now;
            }
            lastSpeechTime = now;
        }

        // Trigger: Silence detected after some speech
        if (isSpeaking && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSpeechTime).count() > SILENCE_MS_TRIGGER) {
            isSpeaking = false;
            
            // Determine how much audio to process (up to 10s)
            int speechDurMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startSpeechTime).count();
            if (speechDurMs > 8000) speechDurMs = 8000; // Limit processing window
            if (speechDurMs < 500) { /* too short, ignore */ }
            else {
                capture.get(speechDurMs + 500, pcmf32); // Include a bit of lead-in
                
                whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
                wparams.n_threads = 4;
                wparams.no_timestamps = true;
                wparams.single_segment = true;
                wparams.print_realtime = false;
                wparams.print_progress = false;

                if (whisper_full(ctx, wparams, pcmf32.data(), (int)pcmf32.size()) == 0) {
                    std::string transcript;
                    int n_segments = whisper_full_n_segments(ctx);
                    for (int i = 0; i < n_segments; ++i) {
                        const char* text = whisper_full_get_segment_text(ctx, i);
                        if (text) transcript += text;
                    }
                    
                    if (!transcript.empty()) {
                        while(!transcript.empty() && (transcript[0] == ' ' || transcript[0] == '.' || transcript[0] == ',')) transcript.erase(0, 1);
                        
                        if (!transcript.empty()) {
                            std::cout << "Voice Command: [" << transcript << "]" << std::endl;
                            
                            std::string lower = transcript;
                            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                            int targetId = -1;
                            
                            // Intent normalization & routing to ToolManager
                            {
                                SpeechIntent si;
                                si.transcript = transcript;
                                si.normalized = lower;
                                // match common intents (simple regexes, case-insensitive already)
                                std::smatch m;
                                // open calculator
                                if (std::regex_search(lower, m, std::regex("\\b(open|launch|start) (the )?(calculator|calc)\\b"))) {
                                    si.intent = "open_calculator";
                                    ToolManager::Instance().HandleIntent(si);
                                    std::lock_guard<std::mutex> lock(g_stateMutex);
                                    g_state.status = "Intent: open_calculator";
                                    // skip goose-targeting when a tool intent matched
                                    goto skip_goose_targeting;
                                }

                                // show calendar
                                if (std::regex_search(lower, m, std::regex("\\b(show|open|view)( the)? calendar\\b"))) {
                                    si.intent = "show_calendar";
                                    ToolManager::Instance().HandleIntent(si);
                                    std::lock_guard<std::mutex> lock(g_stateMutex);
                                    g_state.status = "Intent: show_calendar";
                                    goto skip_goose_targeting;
                                }

                                // schedule meeting
                                if (std::regex_search(lower, m, std::regex("\\b(schedule|set up|book|arrange) (a )?meeting\\b"))) {
                                    si.intent = "schedule_meeting";
                                    ToolManager::Instance().HandleIntent(si);
                                    std::lock_guard<std::mutex> lock(g_stateMutex);
                                    g_state.status = "Intent: schedule_meeting";
                                    goto skip_goose_targeting;
                                }

                                // search web (capture query)
                                if (std::regex_search(lower, m, std::regex("^(search for|google|look up) (.+)$"))) {
                                    si.intent = "search_web";
                                    if (m.size() >= 3) si.entities["query"] = m[2].str();
                                    ToolManager::Instance().HandleIntent(si);
                                    std::lock_guard<std::mutex> lock(g_stateMutex);
                                    g_state.status = "Intent: search_web";
                                    goto skip_goose_targeting;
                                }

                                // open browser
                                if (std::regex_search(lower, m, std::regex("\\b(open|launch) (the )?(browser|chrome|firefox|brave)\\b"))) {
                                    si.intent = "open_browser";
                                    ToolManager::Instance().HandleIntent(si);
                                    std::lock_guard<std::mutex> lock(g_stateMutex);
                                    g_state.status = "Intent: open_browser";
                                    goto skip_goose_targeting;
                                }

                                // set timer
                                if (std::regex_search(lower, m, std::regex("set (a )?timer for (\\d+\\s?(seconds|minutes|hours)?)"))) {
                                    si.intent = "set_timer";
                                    if (m.size() >= 2) si.entities["duration"] = m[2].str();
                                    ToolManager::Instance().HandleIntent(si);
                                    std::lock_guard<std::mutex> lock(g_stateMutex);
                                    g_state.status = "Intent: set_timer";
                                    goto skip_goose_targeting;
                                }

                                // show assignments
                                if (std::regex_search(lower, m, std::regex("\\b(show (my )?(assignments|tasks|todos)|what('?s )?(my )?(tasks|assignments))\\b"))) {
                                    si.intent = "show_assignments";
                                    ToolManager::Instance().HandleIntent(si);
                                    std::lock_guard<std::mutex> lock(g_stateMutex);
                                    g_state.status = "Intent: show_assignments";
                                    goto skip_goose_targeting;
                                }

                                // fallback status
                                if (std::regex_search(lower, m, std::regex("\\b(status|what'?s the status|help)\\b"))) {
                                    si.intent = "status";
                                    ToolManager::Instance().HandleIntent(si);
                                    std::lock_guard<std::mutex> lock(g_stateMutex);
                                    g_state.status = "Intent: status";
                                    goto skip_goose_targeting;
                                }
                            }

                            
                            // 1. Check for specific named geese first (e.g. "Ben")
                            // Scan all geese to see if their name is mentioned
                            for (const auto& g : g_geese) {
                                if (g.name.empty()) continue;
                                std::string gname = g.name;
                                std::transform(gname.begin(), gname.end(), gname.begin(), ::tolower);
                                if (lower.find(gname) != std::string::npos) {
                                    targetId = g.id;
                                    break;
                                }
                            }

                            // 2. Fallback: Check for "Goose X" pattern
                            if (targetId == -1) {
                                std::regex re("goose\\s+(zero|one|two|three|four|five|six|seven|eight|nine|\\d+)");
                                std::smatch match;
                                if (std::regex_search(lower, match, re)) {
                                    std::string val = match[1].str();
                                    if (isdigit(val[0])) targetId = std::stoi(val);
                                    else if (val == "zero") targetId = 0;
                                    else if (val == "one") targetId = 1;
                                    else if (val == "two") targetId = 2;
                                    else if (val == "three") targetId = 3;
                                    else if (val == "four") targetId = 4;
                                    else if (val == "five") targetId = 5;
                                    else if (val == "six") targetId = 6;
                                    else if (val == "seven") targetId = 7;
                                    else if (val == "eight") targetId = 8;
                                    else if (val == "nine") targetId = 9;
                                }
                            }
                            if (targetId != -1) {
                                std::string targetName = "Goose " + std::to_string(targetId);
                                
                                // Try to find the actual name
                                for (const auto& g : g_geese) {
                                    if (g.id == targetId && !g.name.empty()) {
                                        targetName = g.name;
                                        break;
                                    }
                                }

                                std::cout << "Command targeted at " << targetName << " (#" << targetId << ")" << std::endl;
                                std::thread aiThread([transcript, targetId, targetName]() {
                                    std::string prompt = "The user said to a virtual goose named \"" + targetName + "\": \"" + transcript + "\" - Provide a very short, personality-rich response (max 150 chars).";
                                    std::string response = Gemini_Ask(prompt, g_config.geminiApiKey);
                                    
                                    std::cout << "[AI Response] " << response << std::endl;

                                    struct CallbackData { int id; std::string text; };
                                    CallbackData* data = new CallbackData{targetId, response};
                                    
                                    g_idle_add([](gpointer d) -> gboolean {
                                        CallbackData* c = (CallbackData*)d;
                                        Goose* g = GetGooseById(c->id);
                                        if (g) {
                                            g->ForceFetchText(c->text, g_screenWidth, g_screenHeight);
                                        }
                                        delete c;
                                        return FALSE;
                                    }, data);
                                });
                                aiThread.detach();
                            }

                            // Label used for skipping goose-targeting when tool intents matched
                            skip_goose_targeting: ;

                            std::lock_guard<std::mutex> lock(g_stateMutex);
                            g_state.transcript = transcript;
                            g_state.lastUpdateTime = std::chrono::duration<double>(now.time_since_epoch()).count();
                        }
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    whisper_free(ctx);
    capture.pause();
    capture.close();

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state.running = false;
        g_state.status = "Idle";
    }
}

void whisper_manager_shutdown() {
    g_stopRequested = true;
    if (g_worker.joinable()) {
        g_worker.join();
    }
    g_running = false;
}

struct WhisperManagerAtexit {
    WhisperManagerAtexit() { std::atexit(whisper_manager_shutdown); }
} _whisper_manager_at_exit;
}

void WhisperManager_RequestToggle(bool enable) {
    if (enable) {
        if (g_running.load()) return;
        g_stopRequested = false;
        g_worker = std::thread(worker_loop);
        g_running = true;
    } else {
        if (!g_running.load()) return;
        g_stopRequested = true;
        if (g_worker.joinable()) {
            g_worker.join();
        }
        g_running = false;
    }
}

WhisperVisualizerState WhisperManager_Snapshot() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_state;
}

bool WhisperManager_IsCapturing() {
    return g_running.load();
}
