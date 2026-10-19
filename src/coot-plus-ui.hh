/*
 * src/coot-plus-ui.hh
 *
 * Coot+ task-based interface additions: command search (Cmd-K), task sidebar
 * and context inspector.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or (at
 * your option) any later version.
 */

#ifndef COOT_PLUS_UI_HH
#define COOT_PLUS_UI_HH

#include <gtk/gtk.h>

// call once the main window, the actions and the Python modules are set up
void coot_plus_setup_ui(GtkApplication *application);

// show and focus the command search entry
void coot_plus_show_command_search();

// show a message in the message strip above the status bar
// kind: "error", "warning" or "info"; hint: optional second line (what to do)
#include <string>
void coot_plus_message(const std::string &kind, const std::string &text, const std::string &hint = "");

// record a line (Python, or a "# comment") in the command log
void coot_plus_log_command(const std::string &line);

#endif // COOT_PLUS_UI_HH
