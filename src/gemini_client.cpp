#include "gemini_client.h"
#include <curl/curl.h>
#include "../third_party/whisper.cpp/examples/json.hpp"
#include <iostream>

// In unit tests we compile this file without linking the full app's globals.
// Define GEMINI_CLIENT_NO_GLOBAL_CONFIG to disable g_config-based logging.
#ifndef GEMINI_CLIENT_NO_GLOBAL_CONFIG
#include "config.h"
#endif

using json = nlohmann::json;

// libcurl write callback — appends response chunks
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

static std::string RedactKey(const std::string& key) {
    if (key.empty()) return "(empty)";
    if (key.size() <= 8) return "(redacted)";
    return key.substr(0, 4) + "…" + key.substr(key.size() - 4);
}

static std::string ClipForLog(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen) + "…";
}

static bool GeminiDebugEnabled() {
#ifndef GEMINI_CLIENT_NO_GLOBAL_CONFIG
    return g_config.geminiDebug;
#else
    return false;
#endif
}

static bool GeminiLogRawEnabled() {
#ifndef GEMINI_CLIENT_NO_GLOBAL_CONFIG
    return g_config.geminiLogRawResponses;
#else
    return false;
#endif
}

std::string Gemini_ParseResponse(const std::string& response_json) {
    try {
        auto j = json::parse(response_json);

        // Successful responses include "candidates"
        if (j.contains("candidates") && !j["candidates"].empty()) {
            auto& content = j["candidates"][0]["content"];
            if (content.contains("parts") && !content["parts"].empty()) {
                return content["parts"][0]["text"].get<std::string>();
            }
        }

        // Structured API error
        if (j.contains("error")) {
            if (j["error"].contains("message")) {
                return "API Error: " + j["error"]["message"].get<std::string>();
            }
            return "API Error: unknown error format.";
        }

        return "Err: Unexpected response format.";
    } catch (const std::exception& e) {
        return "Err: JSON parse failed. " + std::string(e.what());
    }
}

std::string Gemini_Ask(const std::string& query, const std::string& apiKey) {
    if (apiKey.empty()) return "Err: No API Key provided.";

    CURL* curl = curl_easy_init();
    if (!curl) return "Err: CURL init failed.";

    // Use the "latest Flash" model alias for Google Gemini
    std::string model = "gemini-flash-latest";
    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/"
                      + model + ":generateContent?key=" + apiKey;

    std::string response_string;
    std::string header_string;

    // Build request JSON body using the docs format
    json request_body = {
        { "contents", json::array({
            {
                {"role", "user"},
                {"parts", json::array({ {{"text", query}} })}
            }
        })},
        { "generationConfig",
            {
                {"maxOutputTokens", 1200},
                {"temperature", 0.7}
            }
        }
    };

    std::string json_str = request_body.dump();

    // Set headers
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return "Err: " + std::string(curl_easy_strerror(res));
    }

    if (GeminiDebugEnabled() || GeminiLogRawEnabled()) {
        std::cout << "[Gemini] http=" << http_code
                  << " model=" << model
                  << " key=" << RedactKey(apiKey)
                  << " prompt_len=" << query.size()
                  << " prompt=\"" << ClipForLog(query, 160) << "\""
                  << " resp_len=" << response_string.size()
                  << std::endl;
    }

    if (GeminiLogRawEnabled()) {
        std::cout << "[Gemini Raw Response] " << ClipForLog(response_string, 2000) << std::endl;
    }

    return Gemini_ParseResponse(response_string);
}
