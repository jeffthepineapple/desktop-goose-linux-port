#pragma once

#include <string>
#include <vector>

void Cli_PrintHelp();
bool Cli_PrintHelpTopic(const std::string& topic);
void Cli_PrintHelpAll();
void Cli_PrintNotice(bool success, const std::string& message);
int Cli_PrintResponse(const std::vector<std::string>& args, const std::string& response);
