#pragma once

#include <gtk/gtk.h>
#include <string>
#include <vector>

class Goose;

void AppActions_SetApplication(GtkApplication* app);
void AppActions_EnsureInitialGoose();
Goose* AppActions_SpawnGoose(const std::string& name = "");
void AppActions_ClearGeese();
void AppActions_Quit();
std::string AppActions_GetStatus();
std::string AppActions_HandleCommand(const std::vector<std::string>& args);
