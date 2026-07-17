#pragma once

// Interactive goose shell: line editing, history, tab completion, and inline
// hints, all driven by the command registry. Requires a running daemon and an
// interactive terminal. Returns a process exit code.
int Cli_RunShell();
