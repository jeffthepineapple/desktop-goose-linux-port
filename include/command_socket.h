#pragma once

#include <functional>
#include <string>
#include <vector>

using CommandHandler = std::function<std::string(const std::vector<std::string>&)>;

bool CommandSocket_StartServer(CommandHandler handler, std::string* errorOut = nullptr);
void CommandSocket_StopServer();
bool CommandSocket_Send(const std::vector<std::string>& args, std::string* responseOut, std::string* errorOut = nullptr);
