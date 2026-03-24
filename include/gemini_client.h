#pragma once
#include <string>

std::string Gemini_Ask(const std::string& query, const std::string& apiKey);

// Testable helper: parses Gemini JSON response and extracts text or error.
std::string Gemini_ParseResponse(const std::string& response_json);
