/*
 * src/coot-plus-ui.cc
 *
 * Coot+ task-based interface additions:
 *   - a registry of every command (menubar, toolbar buttons and menus, key bindings)
 *   - command search (Cmd-K / Ctrl-K): find any command by name or synonym, see where
 *     it lives and its shortcut, run it with Return
 *   - task sidebar (Load / Navigate / Maps / Symmetry / Build / Ligands / Validate / Save)
 *   - context inspector: the active residue, map contour sliders in sigma, map radius,
 *     one symmetry toggle, label size
 *
 * Everything here drives the existing actions and API functions - no behaviour is
 * duplicated.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or (at
 * your option) any later version.
 */

#include "graphics-info.h" // first: it includes Python.h

#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#include <gtk/gtk.h>

#include "c-interface.h"
#include "cc-interface.hh"
#include "widget-from-builder.hh"
#include "coot-plus-ui.hh"

namespace {

// ------------------------------------------------------------------------------------
//                                  command registry
// ------------------------------------------------------------------------------------

struct command_t {
   std::string label;     // as shown
   std::string path;      // where it lives, e.g. "File > Fetch" or "Keyboard"
   std::string action;    // "app.xxx" (empty if run is set)
   std::string target;    // string parameter for radio-style actions
   std::string shortcut;  // display text, e.g. "Ctrl-G", "Shift-R"
   std::string keywords;  // lower case synonyms for searching
   int priority = 0;      // search ranking bonus for the commands people usually mean
   std::function<void()> run;
};

GtkApplication *coot_plus_app = nullptr;
std::vector<command_t> commands;
std::map<std::string, int> use_count; // key: label + path

bool updating_widgets = false; // guard: don't act on our own set_active()/set_value()

void add_coot_plus_commands(); // Coot+-only commands (symmetry toggle, label size, panels)
void set_priorities();

std::string to_lower(const std::string &s) {
   std::string r = s;
   std::transform(r.begin(), r.end(), r.begin(), [] (unsigned char c) { return std::tolower(c); });
   return r;
}

// letters and digits only, lower case - for matching menu labels to key binding descriptions
std::string squash(const std::string &s) {
   std::string r;
   for (unsigned char c : s)
      if (std::isalnum(c)) r += std::tolower(c);
   return r;
}

std::string clean_label(const std::string &s_in) {
   std::string s = s_in;
   auto ends_with = [&s] (const std::string &e) {
      return s.length() >= e.length() && s.compare(s.length() - e.length(), e.length(), e) == 0;
   };
   while (!s.empty() && s.back() == ' ') s.pop_back();
   if (ends_with("...")) s.erase(s.length() - 3);
   else if (ends_with("…")) s.erase(s.length() - 3); // "…" is 3 bytes in UTF-8
   // "1: Associate Sequence File" -> "Associate Sequence File"
   if (s.length() > 3 && std::isdigit(static_cast<unsigned char>(s[0])) && s[1] == ':' && s[2] == ' ')
      s = s.substr(3);
   while (!s.empty() && s.back() == ' ') s.pop_back();
   return s;
}

std::string key_display(const keyboard_key_t &k) {
   const char *n = gdk_keyval_name(k.gdk_key);
   std::string name = n ? n : "?";
   static const std::map<std::string, std::string> special = {
      {"bracketleft", "["}, {"bracketright", "]"}, {"quoteleft", "`"}, {"asciitilde", "~"},
      {"plus", "+"}, {"equal", "="}, {"minus", "-"}, {"space", "Space"}, {"Return", "Return"},
      {"Escape", "Esc"}, {"KP_Add", "Keypad +"}, {"KP_Subtract", "Keypad -"}, {"Tab", "Tab"}};
   auto it = special.find(name);
   std::string r;
   if (it != special.end()) {
      r = it->second;
   } else if (name.length() == 1 && std::isalpha(static_cast<unsigned char>(name[0]))) {
      if (std::isupper(static_cast<unsigned char>(name[0])))
         r = std::string("Shift-") + name;
      else
         r = std::string(1, std::toupper(static_cast<unsigned char>(name[0])));
   } else {
      r = name;
   }
   if (k.ctrl_is_pressed) r = "Ctrl-" + r;
   return r;
}

// Extra words people search with (crystallographer vocabulary, other programs' names).
// key: squashed label
const std::map<std::string, std::string> &synonyms() {
   static const std::map<std::string, std::string> m = {
      {"cellsymmetry",              "symmetry symm unit cell crystal packing neighbours trimer lattice"},
      {"undosymmetryview",          "symmetry back"},
      {"savesymmetrycoordinates",   "symmetry write copy export"},
      {"allmoleculesusenearchainssymmetry", "symmetry whole chain"},
      {"symmshiftreferencechainhere", "symmetry move chain"},
      {"gotoatom",                  "goto navigate residue find centre center jump"},
      {"showgotoresiduekeyboardingwindow", "goto navigate residue jump"},
      {"gotoligand",                "goto navigate ligand jump"},
      {"flippeptide",               "pepflip pep flip peptide"},
      {"pepflip",                   "pepflip flip peptide"},
      {"mapparameters",             "contour level sigma radius sampling"},
      {"increasecontourlevel",      "contour sigma map level"},
      {"decreasecontourlevel",      "contour sigma map level"},
      {"increasemapradius",         "map extent box"},
      {"decreasemapradius",         "map extent box"},
      {"savecoordinates",           "write pdb mmcif export save model"},
      {"opencoordinates",           "read load pdb mmcif model"},
      {"automtz",                   "read load mtz map data reflections"},
      {"autoopenmtz",               "read load mtz map data reflections phenix refine"},
      {"openmap",                   "read load ccp4 mrc map"},
      {"openmtzmmciffcforphs",      "read load data reflections"},
      {"findligands",               "ligand fit blob dock"},
      {"getmonomer",                "ligand load dictionary restraints three letter code"},
      {"importcifdictionary",       "ligand restraints load"},
      {"unmodelledblobs",           "blob density ligand missing"},
      {"differencemappeaks",        "peaks fofc fo-fc difference positive negative"},
      {"findwaters",                "water solvent hoh"},
      {"addwater",                  "water hoh solvent"},
      {"addwaterhere",              "water hoh solvent"},
      {"ramachandranplot",          "rama phi psi validation"},
      {"validationgraphs",          "density fit geometry rotamer validation"},
      {"spherrefine",               "refine real space rsr"},
      {"sphererefine",              "refine real space rsr"},
      {"refineresidueautoaccept",   "refine real space rsr"},
      {"triplerefine",              "refine real space rsr"},
      {"chainrefine",               "refine real space rsr"},
      {"rigidbodyfitmolecule",      "rigid body fit"},
      {"rigidbodyfitresidueranges", "rigid body fit"},
      {"simplemutate",              "mutate change residue type"},
      {"keyboardmutate",            "mutate change residue type"},
      {"mutateresiduerange",        "mutate sequence"},
      {"rotamers",                  "rotamer side chain sidechain"},
      {"autofitrotamer",            "rotamer side chain sidechain fit"},
      {"fillpartialresidues",       "missing atoms side chain sidechain"},
      {"fillpartialresidue",        "missing atoms side chain sidechain"},
      {"screenshot",                "picture image figure png"},
      {"savestate",                 "session"},
      {"recoversession",            "session crash restore"},
      {"exportmap",                 "save write map ccp4 mrc"},
      {"labelatomsinresidue",       "label text names"},
      {"clearatomlabels",           "label remove"},
      {"removeallatomlabels",       "label remove clear"},
      {"labelunlabelactiveatom",    "label text name"},
      {"backgroundcolour",          "background color white black"},
      {"sequenceview",              "sequence"},
      {"residueinfo",               "b factor occupancy atom info properties"},
      {"distancesangles",           "measure distance angle"},
      {"environmentdistances",      "contacts hydrogen bonds measure"},
      {"preferences",               "settings options configure"},
      {"python",                    "console scripting shell command line"},
      {"undo",                      "back revert"},
      {"lsqsuperpose",              "align superimpose"},
      {"ssmsuperpose",              "align superimpose secondary structure matching"},
      {"superposeligands",          "align superimpose ligand"},
      {"makeadifferencemap",        "fofc fo-fc subtract"},
      {"xraymapsharpeningblurring", "sharpen blur b-factor"},
      {"exit",                      "quit close"},
   };
   return m;
}

// search ranking bonus: key = squashed label
int priority_for(const std::string &label) {
   static const std::map<std::string, int> m = {
      {"showsymmetry", 60}, {"cellsymmetry", 30}, {"gotoatom", 35}, {"showgotoresiduekeyboardingwindow", 15},
      {"flippeptide", 70}, {"autoopenmtz", 30}, {"opencoordinates", 30}, {"openmap", 20},
      {"findligands", 35}, {"getmonomer", 25}, {"savecoordinates", 30}, {"mapparameters", 25},
      {"sphererefine", 60}, {"refineresidueautoaccept", 15}, {"triplerefine", 10},
      {"validationgraphs", 25}, {"differencemappeaks", 20}, {"unmodelledblobs", 20},
      {"ramachandranplot", 20}, {"python", 25}, {"preferences", 20}, {"screenshot", 15},
      {"labelsizebigger", 25}, {"labelsizesmaller", 20}, {"rotamers", 25}, {"autofitrotamer", 25},
      {"simplemutate", 20}, {"mutateautofit", 25}, {"fillpartialresidues", 20}, {"addwaterhere", 20},
      {"undo", 20}, {"redo", 10}, {"residueinfo", 20}, {"increasecontourlevel", 15},
      {"decreasecontourlevel", 15}, {"scrollwheelzooms", 40}, {"togglescrollwheelzoomcontour", 30}};
   auto it = m.find(squash(label));
   return it == m.end() ? 0 : it->second;
}

std::string keywords_for(const std::string &label) {
   const auto &m = synonyms();
   auto it = m.find(squash(label));
   return it == m.end() ? std::string() : it->second;
}

bool have_command(const std::string &action, const std::string &target) {
   for (const auto &c : commands)
      if (!c.action.empty() && c.action == action && c.target == target) return true;
   return false;
}

void add_menu_model(GMenuModel *model, const std::string &path) {

   int n = g_menu_model_get_n_items(model);
   for (int i=0; i<n; i++) {

      GMenuModel *section = g_menu_model_get_item_link(model, i, G_MENU_LINK_SECTION);
      if (section) {
         add_menu_model(section, path);
         g_object_unref(section);
         continue;
      }

      gchar *label = nullptr;
      g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &label);
      std::string l = label ? clean_label(label) : std::string();
      g_free(label);

      GMenuModel *submenu = g_menu_model_get_item_link(model, i, G_MENU_LINK_SUBMENU);
      if (submenu) {
         add_menu_model(submenu, path.empty() ? l : path + " > " + l);
         g_object_unref(submenu);
         continue;
      }

      gchar *action = nullptr;
      g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_ACTION, "s", &action);
      if (action && !l.empty()) {
         command_t c;
         c.label = l;
         c.path = path;
         c.action = action;
         GVariant *target = g_menu_model_get_item_attribute_value(model, i, G_MENU_ATTRIBUTE_TARGET, NULL);
         if (target) {
            if (g_variant_is_of_type(target, G_VARIANT_TYPE_STRING))
               c.target = g_variant_get_string(target, NULL);
            g_variant_unref(target);
         }
         c.keywords = keywords_for(l);
         if (c.action != "app.command_search_action" && !have_command(c.action, c.target))
            commands.push_back(c);
      }
      g_free(action);
   }
}

// toolbar buttons that are not actions: (builder id, label)
const std::vector<std::pair<std::string, std::string> > &toolbar_buttons() {
   static const std::vector<std::pair<std::string, std::string> > v = {
      {"vertical_toolbar_refine_regularize_button", "Refine/Regularize Control"},
      {"vertical_toolbar_map_button",               "Select Map for Fitting"},
      {"select_residue_range_toggle_button",        "Select Residue Range/Pair"},
      {"vertical_toolbar_testbutton_4",             "Auto-Fit Rotamer"},
      {"vertical_toolbar_testbutton_5",             "Rotamers"},
      {"vertical_toolbar_testbutton_6",             "Edit Chi Angles"},
      {"flip_peptide_button",                       "Flip Peptide"},
      {"side_chain_180_flip_button",                "Sidechain 180° Flip"},
      {"simple_mutate_button",                      "Simple Mutate"},
      {"mutate_and_autofit_menubutton",             "Mutate & Auto-Fit"},
      {"vertical_toolbar_add_terminal_residue_button", "Add Terminal Residue"},
      {"add_alt_conf_button",                       "Add Alt Conf"},
      {"refine_menubutton",                         "Refine Menu"},
      {"delete_menubutton",                         "Delete Menu"},
      {"undo_button",                               "Undo"},
      {"redo_button",                               "Redo"}};
   return v;
}

// toolbar menus: (builder id, path shown)
const std::vector<std::pair<std::string, std::string> > &toolbar_menus() {
   static const std::vector<std::pair<std::string, std::string> > v = {
      {"refine-menu",           "Toolbar > Refine"},
      {"fixed-atoms-menu",      "Toolbar > Fixed Atoms"},
      {"rigid-body-fit-menu",   "Toolbar > Rigid Body Fit"},
      {"rotate-translate-menu", "Toolbar > Rotate/Translate"},
      {"delete-item-menu",      "Toolbar > Delete"},
      {"ligand-menu",           "Ligand"},
      {"restraints-menu",       "Restraints"}};
   return v;
}

void build_command_registry(GtkApplication *application) {

   commands.clear();

   GMenuModel *menubar = gtk_application_get_menubar(application);
   if (menubar)
      add_menu_model(menubar, "");

   for (const auto &tb : toolbar_buttons()) {
      GtkWidget *w = widget_from_builder(tb.first);
      if (!w) continue;
      command_t c;
      c.label = tb.second;
      c.path = "Toolbar";
      c.keywords = keywords_for(tb.second);
      std::string id = tb.first;
      c.run = [id] () {
         GtkWidget *b = widget_from_builder(id);
         if (!b) return;
         if (GTK_IS_MENU_BUTTON(b))
            gtk_menu_button_popup(GTK_MENU_BUTTON(b));
         else
            gtk_widget_activate(b);
      };
      commands.push_back(c);
   }

   for (const auto &tm : toolbar_menus()) {
      GObject *o = graphics_info_t::get_gobject_from_builder(tm.first);
      if (o && G_IS_MENU_MODEL(o))
         add_menu_model(G_MENU_MODEL(o), tm.second);
   }

   // key bindings: give matching commands their shortcut, add the rest as commands
   // aliases: squashed key binding description -> squashed command label
   static const std::map<std::string, std::string> aliases = {
      {"pepflip", "flippeptide"},
      {"showgotoresiduekeyboardingwindow", "gotoresidue"},
      {"autofitrotamer", "autofitrotamer"},
      {"removeallatomlabels", "clearatomlabels"}};

   for (const auto &kb_pair : graphics_info_t::key_bindings_map) {
      const keyboard_key_t &key = kb_pair.first;
      const key_bindings_t &kb  = kb_pair.second;
      if (kb.description.empty()) continue;
      std::string sq = squash(kb.description);
      auto it_alias = aliases.find(sq);
      std::string sq_target = (it_alias == aliases.end()) ? sq : it_alias->second;
      std::string shortcut = key_display(key);
      bool matched = false;
      for (auto &c : commands) {
         if (squash(c.label) == sq_target || squash(c.label) == sq) {
            if (c.shortcut.empty())
               c.shortcut = shortcut;
            else if (c.shortcut.find(shortcut) == std::string::npos)
               c.shortcut += ", " + shortcut;
            matched = true;
         }
      }
      if (matched) continue;
      // one keyboard command per description (several keys can do the same thing)
      bool done = false;
      for (auto &c : commands) {
         if (c.path == "Keyboard" && squash(c.label) == sq) {
            if (c.shortcut.find(shortcut) == std::string::npos)
               c.shortcut += ", " + shortcut;
            done = true;
         }
      }
      if (done) continue;
      command_t c;
      c.label = kb.description;
      c.label[0] = std::toupper(static_cast<unsigned char>(c.label[0]));
      c.path = "Keyboard";
      c.shortcut = shortcut;
      c.keywords = "key keyboard " + keywords_for(kb.description);
      key_bindings_t kb_copy = kb;
      c.run = [kb_copy] () { kb_copy.run(); };
      commands.push_back(c);
   }

   add_coot_plus_commands();
   set_priorities();
}

void set_priorities() {
   for (auto &c : commands)
      if (c.priority == 0) c.priority = priority_for(c.label);
}

const command_t *find_command_by_action(const std::string &action, const std::string &target = "") {
   for (const auto &c : commands)
      if (c.action == action && c.target == target) return &c;
   return nullptr;
}

const command_t *find_command_by_label(const std::string &label, const std::string &path = "") {
   std::string sq = squash(label);
   for (const auto &c : commands)
      if (squash(c.label) == sq && (path.empty() || c.path == path)) return &c;
   return nullptr;
}

GtkWidget *gl_area() {
   if (graphics_info_t::glareas.empty()) return nullptr;
   return graphics_info_t::glareas[0];
}

void give_focus_back_to_graphics() {
   GtkWidget *w = gl_area();
   if (w) gtk_widget_grab_focus(w);
}

void run_command(const command_t &c) {

   use_count[c.label + "|" + c.path]++;

   if (c.run) {
      c.run();
      return;
   }
   std::string name = c.action;
   if (name.rfind("app.", 0) == 0) name = name.substr(4);
   GActionGroup *group = G_ACTION_GROUP(coot_plus_app);
   if (!group || !g_action_group_has_action(group, name.c_str())) {
      graphics_info_t::add_status_bar_text(c.label + " is not available in this build");
      return;
   }
   if (!g_action_group_get_action_enabled(group, name.c_str())) {
      graphics_info_t::add_status_bar_text(c.label + " is not available right now");
      return;
   }
   GVariant *param = nullptr;
   const GVariantType *pt = g_action_group_get_action_parameter_type(group, name.c_str());
   if (pt) {
      if (c.target.empty()) {
         graphics_info_t::add_status_bar_text(c.label + ": use it from its menu (" + c.path + ")");
         return;
      }
      param = g_variant_new_string(c.target.c_str());
   }
   g_action_group_activate_action(group, name.c_str(), param);
}

// ------------------------------------------------------------------------------------
//                                  command search
// ------------------------------------------------------------------------------------

GtkWidget *search_entry   = nullptr;
GtkWidget *search_popover = nullptr;
GtkWidget *search_listbox = nullptr;
std::vector<int> search_results; // indices into commands, in row order

bool is_subsequence(const std::string &needle, const std::string &hay) {
   size_t j = 0;
   for (size_t i=0; i<hay.size() && j<needle.size(); i++)
      if (hay[i] == needle[j]) j++;
   return j == needle.size();
}

bool starts_word(const std::string &hay, size_t pos) {
   return pos == 0 || !std::isalnum(static_cast<unsigned char>(hay[pos-1]));
}

int match_score(const command_t &c, const std::string &query, const std::vector<std::string> &tokens) {

   std::string label = to_lower(c.label);
   std::string path  = to_lower(c.path);
   std::string hay   = label + " " + c.keywords + " " + path;
   int s = 0;
   for (const auto &t : tokens) {
      size_t p = label.find(t);
      if (p != std::string::npos) {
         s += starts_word(label, p) ? 30 : 15;
         continue;
      }
      p = c.keywords.find(t);
      if (p != std::string::npos) { s += starts_word(c.keywords, p) ? 20 : 8; continue; }
      p = path.find(t);
      if (p != std::string::npos) { s += 5; continue; }
      if (t.size() >= 3 && is_subsequence(t, squash(c.label))) { s += 2; continue; }
      return -1;
   }
   s += c.priority;
   if (label.rfind(query, 0) == 0) s += 60;
   else if (label.find(query) != std::string::npos) s += 25;
   auto it = use_count.find(c.label + "|" + c.path);
   if (it != use_count.end()) s += 4 * std::min(it->second, 5);
   if (c.path == "Keyboard") s -= 3;
   return s;
}

GtkWidget *make_result_row(const command_t &c) {

   GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
   gtk_widget_set_margin_start(row_box, 8);
   gtk_widget_set_margin_end(row_box, 8);
   gtk_widget_set_margin_top(row_box, 4);
   gtk_widget_set_margin_bottom(row_box, 4);

   GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
   GtkWidget *name = gtk_label_new(c.label.c_str());
   gtk_label_set_xalign(GTK_LABEL(name), 0.0);
   gtk_widget_set_hexpand(name, TRUE);
   gtk_widget_add_css_class(name, "heading");
   gtk_box_append(GTK_BOX(top), name);
   if (!c.shortcut.empty()) {
      GtkWidget *sc = gtk_label_new(c.shortcut.c_str());
      gtk_widget_add_css_class(sc, "dim-label");
      gtk_widget_add_css_class(sc, "coot-plus-shortcut");
      gtk_box_append(GTK_BOX(top), sc);
   }
   gtk_box_append(GTK_BOX(row_box), top);

   std::string where = c.path.empty() ? std::string("Menu") : c.path;
   GtkWidget *path = gtk_label_new(where.c_str());
   gtk_label_set_xalign(GTK_LABEL(path), 0.0);
   gtk_widget_add_css_class(path, "dim-label");
   gtk_widget_add_css_class(path, "caption");
   gtk_box_append(GTK_BOX(row_box), path);

   GtkWidget *row = gtk_list_box_row_new();
   gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
   gtk_widget_set_focusable(row, FALSE); // keep the keyboard focus in the entry
   return row;
}

void update_search_results() {

   if (!search_listbox) return;
   GtkWidget *child;
   while ((child = gtk_widget_get_first_child(search_listbox)) != nullptr)
      gtk_list_box_remove(GTK_LIST_BOX(search_listbox), child);
   search_results.clear();

   std::string query = to_lower(gtk_editable_get_text(GTK_EDITABLE(search_entry)));
   while (!query.empty() && query.back()  == ' ') query.pop_back();
   while (!query.empty() && query.front() == ' ') query.erase(0, 1);

   std::vector<std::pair<int, int> > scored; // score, index
   if (query.empty()) {
      // recently used first, then a few common ones
      for (unsigned int i=0; i<commands.size(); i++) {
         auto it = use_count.find(commands[i].label + "|" + commands[i].path);
         if (it != use_count.end()) scored.push_back(std::make_pair(100 + it->second, i));
      }
      for (const auto &a : {"app.open_coordinates_action", "app.auto_open_mtz_action", "app.go_to_atom_action",
                            "app.draw_cell_and_symmetry_action", "app.find_ligands_action",
                            "app.save_coordinates_action", "app.show_validation_graphs_dialog"}) {
         for (unsigned int i=0; i<commands.size(); i++)
            if (commands[i].action == a && use_count.find(commands[i].label + "|" + commands[i].path) == use_count.end())
               scored.push_back(std::make_pair(1, i));
      }
   } else {
      std::vector<std::string> tokens;
      std::string t;
      for (char ch : query) {
         if (ch == ' ') { if (!t.empty()) tokens.push_back(t); t.clear(); }
         else t += ch;
      }
      if (!t.empty()) tokens.push_back(t);
      for (unsigned int i=0; i<commands.size(); i++) {
         int s = match_score(commands[i], query, tokens);
         if (s >= 0) scored.push_back(std::make_pair(s, i));
      }
   }
   std::stable_sort(scored.begin(), scored.end(),
                    [] (const std::pair<int,int> &a, const std::pair<int,int> &b) { return a.first > b.first; });

   const unsigned int max_rows = 14;
   for (unsigned int i=0; i<scored.size() && i<max_rows; i++) {
      search_results.push_back(scored[i].second);
      gtk_list_box_append(GTK_LIST_BOX(search_listbox), make_result_row(commands[scored[i].second]));
   }
   if (search_results.empty()) {
      GtkWidget *l = gtk_label_new(query.empty() ? "Type to search every command" : "No matching command");
      gtk_widget_add_css_class(l, "dim-label");
      gtk_widget_set_margin_top(l, 10);
      gtk_widget_set_margin_bottom(l, 10);
      GtkWidget *row = gtk_list_box_row_new();
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), l);
      gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
      gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
      gtk_widget_set_focusable(row, FALSE);
      gtk_list_box_append(GTK_LIST_BOX(search_listbox), row);
   } else {
      gtk_list_box_select_row(GTK_LIST_BOX(search_listbox),
                              gtk_list_box_get_row_at_index(GTK_LIST_BOX(search_listbox), 0));
   }
}

void hide_search() {
   if (search_popover) gtk_popover_popdown(GTK_POPOVER(search_popover));
}

void finish_search_and_run(int row_index) {
   if (row_index < 0 || row_index >= static_cast<int>(search_results.size())) return;
   command_t c = commands[search_results[row_index]]; // copy: running may rebuild things
   hide_search();
   gtk_editable_set_text(GTK_EDITABLE(search_entry), "");
   give_focus_back_to_graphics();
   run_command(c);
}

void move_search_selection(int delta) {
   if (search_results.empty()) return;
   GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(search_listbox));
   int idx = row ? gtk_list_box_row_get_index(row) : -1;
   idx = std::max(0, std::min(static_cast<int>(search_results.size()) - 1, idx + delta));
   gtk_list_box_select_row(GTK_LIST_BOX(search_listbox),
                           gtk_list_box_get_row_at_index(GTK_LIST_BOX(search_listbox), idx));
}

void show_search_results() {
   update_search_results();
   if (search_popover) gtk_popover_popup(GTK_POPOVER(search_popover));
}

GtkWidget *make_search_entry() {

   search_entry = gtk_search_entry_new();
   gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search_entry), "Search any tool  (⌘K)");
   gtk_editable_set_width_chars(GTK_EDITABLE(search_entry), 34);
   gtk_widget_set_tooltip_text(search_entry, "Find any command by name, e.g. \"symmetry\", \"pepflip\", \"refine\"");
   gtk_widget_set_valign(search_entry, GTK_ALIGN_CENTER);
   gtk_widget_set_margin_start(search_entry, 12);
   gtk_widget_set_margin_end(search_entry, 12);

   search_listbox = gtk_list_box_new();
   gtk_list_box_set_selection_mode(GTK_LIST_BOX(search_listbox), GTK_SELECTION_BROWSE);
   gtk_widget_set_focusable(search_listbox, FALSE);
   g_signal_connect(search_listbox, "row-activated",
                    G_CALLBACK(+[] (GtkListBox *, GtkListBoxRow *row, gpointer) {
                       finish_search_and_run(gtk_list_box_row_get_index(row));
                    }), nullptr);

   GtkWidget *scrolled = gtk_scrolled_window_new();
   gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), 480);
   gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);
   gtk_widget_set_size_request(scrolled, 440, -1);
   gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), search_listbox);

   search_popover = gtk_popover_new();
   gtk_popover_set_child(GTK_POPOVER(search_popover), scrolled);
   gtk_popover_set_autohide(GTK_POPOVER(search_popover), FALSE); // the entry keeps the focus
   gtk_popover_set_has_arrow(GTK_POPOVER(search_popover), FALSE);
   gtk_popover_set_position(GTK_POPOVER(search_popover), GTK_POS_BOTTOM);
   gtk_widget_set_parent(search_popover, search_entry);

   g_signal_connect(search_entry, "search-changed",
                    G_CALLBACK(+[] (GtkSearchEntry *, gpointer) { show_search_results(); }), nullptr);
   g_signal_connect(search_entry, "activate",
                    G_CALLBACK(+[] (GtkSearchEntry *, gpointer) {
                       GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(search_listbox));
                       finish_search_and_run(row ? gtk_list_box_row_get_index(row) : 0);
                    }), nullptr);
   g_signal_connect(search_entry, "stop-search",
                    G_CALLBACK(+[] (GtkSearchEntry *, gpointer) {
                       hide_search();
                       gtk_editable_set_text(GTK_EDITABLE(search_entry), "");
                       give_focus_back_to_graphics();
                    }), nullptr);

   GtkEventController *keys = gtk_event_controller_key_new();
   gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
   g_signal_connect(keys, "key-pressed",
                    G_CALLBACK(+[] (GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer) -> gboolean {
                       if (keyval == GDK_KEY_Down) { move_search_selection(1);  return TRUE; }
                       if (keyval == GDK_KEY_Up)   { move_search_selection(-1); return TRUE; }
                       return FALSE;
                    }), nullptr);
   gtk_widget_add_controller(search_entry, keys);

   GtkEventController *focus = gtk_event_controller_focus_new();
   g_signal_connect(focus, "enter", G_CALLBACK(+[] (GtkEventControllerFocus *, gpointer) { show_search_results(); }), nullptr);
   g_signal_connect(focus, "leave", G_CALLBACK(+[] (GtkEventControllerFocus *, gpointer) { hide_search(); }), nullptr);
   gtk_widget_add_controller(search_entry, focus);

   return search_entry;
}

void add_command_search_shortcut(GtkWidget *window) {
   GtkEventController *sc = gtk_shortcut_controller_new();
   gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(sc), GTK_SHORTCUT_SCOPE_GLOBAL);
   gtk_event_controller_set_propagation_phase(sc, GTK_PHASE_CAPTURE);
   GtkShortcutTrigger *trigger = gtk_shortcut_trigger_parse_string("<Meta>k|<Control>k");
   GtkShortcutAction *action = gtk_callback_action_new(+[] (GtkWidget *, GVariant *, gpointer) -> gboolean {
                                                          coot_plus_show_command_search();
                                                          return TRUE;
                                                       }, nullptr, nullptr);
   gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc), gtk_shortcut_new(trigger, action));
   gtk_widget_add_controller(window, sc);
}

// ------------------------------------------------------------------------------------
//                                  symmetry (one toggle)
// ------------------------------------------------------------------------------------

bool symmetry_is_on() {
   return get_show_symmetry() != 0;
}

void set_symmetry_view(bool state) {
   if (state) {
      set_show_symmetry_master(1);
      int n_models = 0, n_with_cell = 0;
      for (int imol=0; imol<graphics_n_molecules(); imol++) {
         if (is_valid_model_molecule(imol)) {
            n_models++;
            if (has_unit_cell_state(imol)) n_with_cell++;
            set_show_symmetry_molecule(imol, 1);
            symmetry_as_calphas(imol, 0); // full atoms
         }
      }
      if (graphics_info_t::symmetry_search_radius < 20.0)
         set_symmetry_size(40.0);
      std::string m;
      if (n_models == 0)
         m = "Symmetry on - open a model with a unit cell (CRYST1) to see its neighbours";
      else if (n_with_cell == 0)
         m = "Symmetry on, but no model has a unit cell (CRYST1 record) - nothing to show";
      else if (!getenv("SYMINFO"))
         m = "Symmetry on, but no symmetry library (syminfo.lib) was found";
      else
         m = "Symmetry on: showing neighbours within " +
            std::to_string(static_cast<int>(graphics_info_t::symmetry_search_radius + 0.5)) + " Å";
      graphics_info_t::add_status_bar_text(m);
   } else {
      set_show_symmetry_master(0);
      graphics_info_t::add_status_bar_text("Symmetry off");
   }
   graphics_draw();
}

// ------------------------------------------------------------------------------------
//                       scroll wheel: zoom or map contour (remembered)
// ------------------------------------------------------------------------------------

GtkWidget *scroll_zoom_button    = nullptr;
GtkWidget *scroll_contour_button = nullptr;

std::string coot_plus_settings_file() {
   return std::string(g_get_user_config_dir()) + "/coot-plus/settings.ini";
}

void save_coot_plus_setting_bool(const char *key, bool value) {
   std::string fn = coot_plus_settings_file();
   GKeyFile *kf = g_key_file_new();
   g_key_file_load_from_file(kf, fn.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr); // ok if missing
   g_key_file_set_boolean(kf, "interface", key, value);
   gchar *dir = g_path_get_dirname(fn.c_str());
   g_mkdir_with_parents(dir, 0755);
   g_free(dir);
   GError *error = nullptr;
   if (!g_key_file_save_to_file(kf, fn.c_str(), &error)) {
      g_warning("coot-plus: could not save %s: %s", fn.c_str(), error->message);
      g_error_free(error);
   }
   g_key_file_free(kf);
}

bool load_coot_plus_setting_bool(const char *key, bool default_value) {
   GKeyFile *kf = g_key_file_new();
   bool r = default_value;
   if (g_key_file_load_from_file(kf, coot_plus_settings_file().c_str(), G_KEY_FILE_NONE, nullptr)) {
      GError *error = nullptr;
      gboolean v = g_key_file_get_boolean(kf, "interface", key, &error);
      if (error) g_error_free(error); else r = v;
   }
   g_key_file_free(kf);
   return r;
}

void sync_scroll_buttons() {
   bool z = get_scroll_wheel_zooms();
   updating_widgets = true;
   if (scroll_zoom_button && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(scroll_zoom_button)) != z)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(scroll_zoom_button), z);
   if (scroll_contour_button && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(scroll_contour_button)) == z)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(scroll_contour_button), !z);
   updating_widgets = false;
}

void set_scroll_mode(bool zooms) {
   set_scroll_wheel_zooms(zooms);
   save_coot_plus_setting_bool("scroll-wheel-zooms", zooms);
   sync_scroll_buttons();
}

GtkWidget *make_scroll_mode_switch() {
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
   gtk_widget_add_css_class(box, "linked");
   gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
   gtk_widget_set_margin_start(box, 8);
   gtk_widget_set_tooltip_text(box, "What the mouse scroll wheel does in the 3D view.\n"
                               "Hold Shift while scrolling to do the other one.");
   GtkWidget *label = gtk_label_new("Scroll");
   gtk_widget_add_css_class(label, "dim-label");
   gtk_widget_set_margin_end(label, 6);
   scroll_zoom_button    = gtk_toggle_button_new_with_label("Zoom");
   scroll_contour_button = gtk_toggle_button_new_with_label("Contour");
   gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(scroll_contour_button), GTK_TOGGLE_BUTTON(scroll_zoom_button));
   for (GtkWidget *b : {scroll_zoom_button, scroll_contour_button}) {
      gtk_widget_set_focusable(b, FALSE);
      g_signal_connect(b, "toggled", G_CALLBACK(+[] (GtkToggleButton *t, gpointer) {
                          if (updating_widgets || !gtk_toggle_button_get_active(t)) return;
                          set_scroll_mode(GTK_WIDGET(t) == scroll_zoom_button);
                       }), nullptr);
   }
   GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
   gtk_widget_set_valign(outer, GTK_ALIGN_CENTER);
   gtk_box_append(GTK_BOX(outer), label);
   gtk_box_append(GTK_BOX(box), scroll_zoom_button);
   gtk_box_append(GTK_BOX(box), scroll_contour_button);
   gtk_box_append(GTK_BOX(outer), box);
   gtk_widget_set_margin_start(outer, 8);
   sync_scroll_buttons();
   return outer;
}

GtkWidget *sidebar_panel   = nullptr; // set in coot_plus_setup_ui()
GtkWidget *inspector_panel = nullptr;

void step_label_size(int delta) {
   int fs = get_font_size() + delta;
   if (fs < 1) fs = 1;
   if (fs > 5) fs = 5;
   set_font_size(fs);
   graphics_draw();
}

void add_coot_plus_commands() {
   auto add = [] (const std::string &label, const std::string &path, const std::string &keywords,
                  std::function<void()> f) {
      command_t c;
      c.label = label; c.path = path; c.keywords = keywords; c.run = f;
      commands.push_back(c);
   };
   add("Show Symmetry", "Inspector > Symmetry",
       "symmetry symm on off toggle trimer crystal neighbours packing lattice copies",
       [] () { set_symmetry_view(!symmetry_is_on()); });
   add("Label Size Bigger", "Inspector > Labels", "font text label size larger bigger increase",
       [] () { step_label_size(1); });
   add("Label Size Smaller", "Inspector > Labels", "font text label size smaller decrease",
       [] () { step_label_size(-1); });
   add("Scroll Wheel Zooms", "Toolbar > Scroll", "mouse scroll wheel zoom in out magnify",
       [] () { set_scroll_mode(true); });
   add("Scroll Wheel Changes Map Contour", "Toolbar > Scroll", "mouse scroll wheel contour level sigma map",
       [] () { set_scroll_mode(false); });
   add("Toggle Scroll Wheel Zoom/Contour", "Toolbar > Scroll", "mouse scroll wheel zoom contour switch",
       [] () { set_scroll_mode(!get_scroll_wheel_zooms()); });
   add("Show/Hide Task Sidebar", "Toolbar > Tasks", "panel sidebar tasks workspace",
       [] () { if (sidebar_panel) gtk_widget_set_visible(sidebar_panel, !gtk_widget_get_visible(sidebar_panel)); });
   add("Show/Hide Inspector", "Toolbar > Inspector", "panel inspector properties",
       [] () { if (inspector_panel) gtk_widget_set_visible(inspector_panel, !gtk_widget_get_visible(inspector_panel)); });
}

// ------------------------------------------------------------------------------------
//                                  task sidebar
// ------------------------------------------------------------------------------------

struct task_t {
   std::string name;
   std::string hint;
   std::vector<std::string> tools; // "app.action[=target]", "kb:Description", "tb:Label", "special:..."
};

const std::vector<task_t> &tasks() {
   static const std::vector<task_t> v = {
      {"Load", "Open models, data and maps", {
            "app.open_coordinates_action", "app.auto_open_mtz_action", "app.open_dataset_action",
            "app.open_map_action", "app.show_accession_code_fetch_frame_oca",
            "app.show_accession_code_fetch_frame_eds", "app.show_accession_code_fetch_frame_uniprot_id",
            "app.get_monomer_action", "app.import_cif_dictionary_action", "app.recover_session_action",
            "app.load_tutorial_model_and_data_action"}},
      {"Navigate", "Move around the model", {
            "app.go_to_atom_action", "kb:Show Go To Residue Keyboarding Window", "kb:Go To Ligand",
            "kb:go to blob", "app.undo_last_navigation_action", "app.sequence_view_action",
            "app.display_only_active_action", "app.residue_type_selection_action"}},
      {"Maps", "Contour, radius and map tools", {
            "special:maps-hint", "app.map_parameters_action", "kb:Increase Map Radius", "kb:Decrease Map Radius",
            "kb:Display Next Map", "kb:Change Scroll-wheel map", "app.make_a_difference_map_action",
            "app.set_map_is_difference_map_action", "app.sharpen_blur_for_xray_action",
            "app.another_contour_level_action", "app.mask_map_by_atom_selection_action"}},
      {"Symmetry", "Crystal neighbours and unit cell", {
            "special:symmetry-toggle", "app.draw_cell_and_symmetry_action", "app.undo_symmetry_view_action",
            "app.all_molecules_near_chains_symmetry_action", "app.save_symmetry_coordinates_action",
            "app.symm_shift_reference_chain_here_action"}},
      {"Build", "Fit and fix the model", {
            "kb:Sphere Refine", "kb:Refine Residue Auto-Accept", "tb:Refine Menu", "tb:Auto-Fit Rotamer",
            "tb:Rotamers", "tb:Flip Peptide", "tb:Sidechain 180° Flip", "tb:Mutate & Auto-Fit",
            "tb:Simple Mutate", "app.fill_partial_residues_action", "tb:Add Terminal Residue",
            "tb:Add Alt Conf", "kb:Add Water Here", "app.rigid_body_fit_residue_ranges_action",
            "app.fit_loop_by_database_search", "tb:Delete Menu", "tb:Undo", "tb:Redo"}},
      {"Ligands", "Find, fit and check ligands", {
            "app.get_monomer_action", "app.find_ligands_action", "kb:Go To Ligand", "app.ligand_builder_action",
            "app.superpose_ligands_action", "app.fetch_pdbe_ligand_description_action",
            "app.fetch_ligand_restraints_from_github_action", "app.import_cif_dictionary_action",
            "app.unmodelled_blobs_action"}},
      {"Validate", "Find what needs fixing", {
            "app.show_validation_graphs_dialog", "app.ramachandran_plot_action",
            "app.validation_outliers_action", "app.difference_map_peaks_action", "app.unmodelled_blobs_action",
            "app.overlaps_peptides_cbeta_ramas_and_rotas_action", "app.pepflips_from_difference_map_action",
            "app.check_delete_waters_action", "app.atoms_overlaps_action",
            "app.all_atom_contact_dots_molprobity_action"}},
      {"Save", "Write models, maps and pictures", {
            "app.save_coordinates_action", "app.save_symmetry_coordinates_action", "app.save_state_action",
            "app.screenshot_action", "app.file_export_map_action", "app.file_export_map_fragment_action"}}
   };
   return v;
}

const command_t *resolve_tool(const std::string &ref) {
   if (ref.rfind("kb:", 0) == 0)
      return find_command_by_label(ref.substr(3));
   if (ref.rfind("tb:", 0) == 0)
      return find_command_by_label(ref.substr(3), "Toolbar");
   std::string action = ref, target;
   size_t eq = ref.find('=');
   if (eq != std::string::npos) { action = ref.substr(0, eq); target = ref.substr(eq+1); }
   return find_command_by_action(action, target);
}

GtkWidget *symmetry_switch_sidebar   = nullptr;
GtkWidget *symmetry_switch_inspector = nullptr;

GtkWidget *make_tool_button(const command_t &c, const std::string &short_label = "", bool show_shortcut = true) {

   GtkWidget *button = gtk_button_new();
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   GtkWidget *l = gtk_label_new(short_label.empty() ? c.label.c_str() : short_label.c_str());
   gtk_label_set_xalign(GTK_LABEL(l), 0.0);
   gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
   gtk_label_set_max_width_chars(GTK_LABEL(l), 20); // keep the sidebar narrow
   gtk_widget_set_hexpand(l, TRUE);
   gtk_box_append(GTK_BOX(box), l);
   if (show_shortcut && !c.shortcut.empty()) {
      std::string first_key = c.shortcut.substr(0, c.shortcut.find(','));
      GtkWidget *sc = gtk_label_new(first_key.c_str());
      gtk_widget_add_css_class(sc, "dim-label");
      gtk_widget_add_css_class(sc, "coot-plus-shortcut");
      gtk_box_append(GTK_BOX(box), sc);
   }
   gtk_button_set_child(GTK_BUTTON(button), box);
   gtk_widget_add_css_class(button, "flat");
   gtk_widget_set_focusable(button, FALSE); // keep keyboard shortcuts going to the graphics
   std::string tip = c.path.empty() ? c.label : c.path + " > " + c.label;
   if (!c.shortcut.empty()) tip += "   (" + c.shortcut + ")";
   gtk_widget_set_tooltip_text(button, tip.c_str());

   command_t *copy = new command_t(c);
   g_signal_connect_data(button, "clicked",
                         G_CALLBACK(+[] (GtkButton *, gpointer data) {
                            run_command(*static_cast<command_t *>(data));
                         }),
                         copy,
                         +[] (gpointer data, GClosure *) { delete static_cast<command_t *>(data); },
                         GConnectFlags(0));
   return button;
}

GtkWidget *make_symmetry_switch_row(GtkWidget **switch_store) {
   GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
   gtk_widget_set_margin_top(row, 4);
   gtk_widget_set_margin_bottom(row, 4);
   gtk_widget_set_margin_start(row, 6);
   GtkWidget *l = gtk_label_new("Show symmetry");
   gtk_label_set_xalign(GTK_LABEL(l), 0.0);
   gtk_widget_set_hexpand(l, TRUE);
   gtk_widget_set_tooltip_text(row, "Show crystal neighbours as full atoms (master switch, every model, radius)");
   GtkWidget *sw = gtk_switch_new();
   gtk_widget_set_focusable(sw, FALSE);
   gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
   gtk_switch_set_active(GTK_SWITCH(sw), symmetry_is_on());
   g_signal_connect(sw, "notify::active",
                    G_CALLBACK(+[] (GObject *o, GParamSpec *, gpointer) {
                       if (updating_widgets) return;
                       set_symmetry_view(gtk_switch_get_active(GTK_SWITCH(o)));
                    }), nullptr);
   gtk_box_append(GTK_BOX(row), l);
   gtk_box_append(GTK_BOX(row), sw);
   *switch_store = sw;
   return row;
}

GtkWidget *sidebar = nullptr;

GtkWidget *make_task_sidebar() {

   GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
   gtk_widget_add_css_class(outer, "coot-plus-sidebar");
   gtk_widget_set_size_request(outer, 200, -1);

   GtkWidget *stack = gtk_stack_new();
   gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
   gtk_stack_set_transition_duration(GTK_STACK(stack), 120);

   GtkWidget *task_list = gtk_list_box_new();
   gtk_list_box_set_selection_mode(GTK_LIST_BOX(task_list), GTK_SELECTION_BROWSE);
   gtk_widget_add_css_class(task_list, "navigation-sidebar");
   gtk_widget_set_focusable(task_list, FALSE);

   for (const auto &task : tasks()) {

      GtkWidget *row_label = gtk_label_new(task.name.c_str());
      gtk_label_set_xalign(GTK_LABEL(row_label), 0.0);
      gtk_widget_set_margin_start(row_label, 6);
      gtk_widget_set_tooltip_text(row_label, task.hint.c_str());
      GtkWidget *row = gtk_list_box_row_new();
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_label);
      gtk_widget_set_focusable(row, FALSE);
      gtk_widget_set_name(row, task.name.c_str());
      gtk_list_box_append(GTK_LIST_BOX(task_list), row);

      GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_set_margin_start(page, 4);
      gtk_widget_set_margin_end(page, 4);
      gtk_widget_set_margin_top(page, 6);
      GtkWidget *hint = gtk_label_new(task.hint.c_str());
      gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
      gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
      gtk_label_set_max_width_chars(GTK_LABEL(hint), 24);
      gtk_widget_add_css_class(hint, "dim-label");
      gtk_widget_add_css_class(hint, "caption");
      gtk_widget_set_margin_start(hint, 6);
      gtk_widget_set_margin_bottom(hint, 4);
      gtk_box_append(GTK_BOX(page), hint);

      for (const auto &ref : task.tools) {
         if (ref == "special:symmetry-toggle") {
            gtk_box_append(GTK_BOX(page), make_symmetry_switch_row(&symmetry_switch_sidebar));
            continue;
         }
         if (ref == "special:maps-hint") {
            GtkWidget *l = gtk_label_new("Contour sliders are in the inspector →");
            gtk_label_set_xalign(GTK_LABEL(l), 0.0);
            gtk_label_set_wrap(GTK_LABEL(l), TRUE);
            gtk_widget_add_css_class(l, "caption");
            gtk_widget_set_margin_start(l, 6);
            gtk_widget_set_margin_bottom(l, 4);
            gtk_box_append(GTK_BOX(page), l);
            continue;
         }
         const command_t *c = resolve_tool(ref);
         if (c)
            gtk_box_append(GTK_BOX(page), make_tool_button(*c));
         else
            g_debug("coot-plus: task tool %s not found", ref.c_str());
      }

      GtkWidget *scrolled = gtk_scrolled_window_new();
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
      gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), page);
      gtk_stack_add_named(GTK_STACK(stack), scrolled, task.name.c_str());
   }

   g_signal_connect(task_list, "row-selected",
                    G_CALLBACK(+[] (GtkListBox *, GtkListBoxRow *row, gpointer stack) {
                       if (row)
                          gtk_stack_set_visible_child_name(GTK_STACK(stack), gtk_widget_get_name(GTK_WIDGET(row)));
                    }), stack);
   gtk_list_box_select_row(GTK_LIST_BOX(task_list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(task_list), 0));

   gtk_box_append(GTK_BOX(outer), task_list);
   gtk_box_append(GTK_BOX(outer), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
   gtk_widget_set_vexpand(stack, TRUE);
   gtk_box_append(GTK_BOX(outer), stack);

   sidebar = outer;
   return outer;
}

// ------------------------------------------------------------------------------------
//                                  context inspector
// ------------------------------------------------------------------------------------

GtkWidget *inspector             = nullptr;
GtkWidget *sel_title_label       = nullptr;
GtkWidget *sel_detail_label      = nullptr;
GtkWidget *maps_box              = nullptr;
GtkWidget *map_radius_scale      = nullptr;
GtkWidget *map_radius_label      = nullptr;
GtkWidget *symm_radius_scale     = nullptr;
GtkWidget *symm_radius_label     = nullptr;
GtkWidget *unit_cell_check       = nullptr;
GtkWidget *label_size_label      = nullptr;
std::string maps_signature;
std::string selection_signature;

struct map_row_t {
   int imol;
   GtkWidget *scale;
   GtkWidget *readout;
   GtkWidget *shown;
};
std::vector<map_row_t> map_rows;

GtkWidget *section_heading(const std::string &text) {
   GtkWidget *l = gtk_label_new(text.c_str());
   gtk_label_set_xalign(GTK_LABEL(l), 0.0);
   gtk_widget_add_css_class(l, "heading");
   gtk_widget_set_margin_top(l, 10);
   return l;
}

std::string residue_display_name(const std::string &three_letter) {
   // "HIS" -> "His" for amino acids, everything else unchanged
   static const std::set<std::string> aa = {"ALA","ARG","ASN","ASP","CYS","GLN","GLU","GLY","HIS","ILE",
                                            "LEU","LYS","MET","PHE","PRO","SER","THR","TRP","TYR","VAL","MSE"};
   if (aa.find(three_letter) == aa.end()) return three_letter;
   std::string r = three_letter;
   for (unsigned int i=1; i<r.size(); i++) r[i] = std::tolower(static_cast<unsigned char>(r[i]));
   return r;
}

std::string file_basename(const std::string &p) {
   size_t s = p.find_last_of('/');
   return s == std::string::npos ? p : p.substr(s+1);
}

std::string format_float(double v, int decimals) {
   char buf[32];
   snprintf(buf, sizeof(buf), "%.*f", decimals, v);
   return buf;
}

void update_selection_section() {

   std::pair<bool, std::pair<int, coot::atom_spec_t> > aa = graphics_info_t::active_atom_spec();
   std::string title = "Nothing selected";
   std::string detail = "Centre on an atom to see it here";
   if (aa.first) {
      int imol = aa.second.first;
      const coot::atom_spec_t &spec = aa.second.second;
      if (is_valid_model_molecule(imol)) {
         mmdb::Atom *at = graphics_info_t::molecules[imol].get_atom(spec);
         if (at) {
            std::string res_name = at->GetResName();
            title = residue_display_name(res_name) + " " + std::to_string(spec.res_no) + spec.ins_code +
               "  chain " + spec.chain_id;
            std::string atom_name = spec.atom_name;
            while (!atom_name.empty() && atom_name.front() == ' ') atom_name.erase(0, 1);
            while (!atom_name.empty() && atom_name.back()  == ' ') atom_name.pop_back();
            detail = "atom " + atom_name;
            if (!spec.alt_conf.empty()) detail += " (alt " + spec.alt_conf + ")";
            detail += "   B " + format_float(at->tempFactor, 1) + "   occ " + format_float(at->occupancy, 2);
            detail += "\n" + file_basename(molecule_name(imol)) + "  (#" + std::to_string(imol) + ")";
         }
      }
   }
   std::string sig = title + "|" + detail;
   if (sig == selection_signature) return;
   selection_signature = sig;
   gtk_label_set_text(GTK_LABEL(sel_title_label), title.c_str());
   gtk_label_set_text(GTK_LABEL(sel_detail_label), detail.c_str());
}

void update_map_readout(const map_row_t &r) {
   float s = get_contour_level_in_sigma(r.imol);
   float a = get_contour_level_absolute(r.imol);
   bool diff = map_is_difference_map(r.imol);
   std::string t = (diff ? "±" : "") + format_float(s, 2) + " σ   " + format_float(a, 3) + " e/Å³";
   gtk_label_set_text(GTK_LABEL(r.readout), t.c_str());
}

void rebuild_maps_section() {

   GtkWidget *child;
   while ((child = gtk_widget_get_first_child(maps_box)) != nullptr)
      gtk_box_remove(GTK_BOX(maps_box), child);
   map_rows.clear();

   int n_maps = 0;
   for (int imol=0; imol<graphics_n_molecules(); imol++) {
      if (!is_valid_map_molecule(imol)) continue;
      n_maps++;
      bool diff = map_is_difference_map(imol);

      GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
      GtkWidget *shown = gtk_check_button_new();
      gtk_widget_set_focusable(shown, FALSE);
      gtk_check_button_set_active(GTK_CHECK_BUTTON(shown), map_is_displayed(imol));
      gtk_widget_set_tooltip_text(shown, "Show/hide this map");
      std::string name = file_basename(molecule_name(imol));
      GtkWidget *name_label = gtk_label_new(((diff ? "Δ " : "") + name).c_str());
      gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
      gtk_label_set_ellipsize(GTK_LABEL(name_label), PANGO_ELLIPSIZE_MIDDLE);
      gtk_label_set_max_width_chars(GTK_LABEL(name_label), 24);
      gtk_widget_set_hexpand(name_label, TRUE);
      gtk_widget_set_tooltip_text(name_label, molecule_name(imol));
      gtk_box_append(GTK_BOX(top), shown);
      gtk_box_append(GTK_BOX(top), name_label);

      double lo = diff ? 1.0 : 0.1, hi = diff ? 8.0 : 5.0;
      GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, hi, 0.05);
      gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
      gtk_widget_set_focusable(scale, FALSE);
      gtk_range_set_value(GTK_RANGE(scale), get_contour_level_in_sigma(imol));
      gtk_widget_set_tooltip_text(scale, "Contour level in σ (same scale for every map)");

      GtkWidget *readout = gtk_label_new("");
      gtk_label_set_xalign(GTK_LABEL(readout), 1.0);
      gtk_widget_add_css_class(readout, "caption");
      gtk_widget_add_css_class(readout, "numeric");

      map_rows.push_back(map_row_t{imol, scale, readout, shown});
      update_map_readout(map_rows.back());

      g_signal_connect(scale, "value-changed",
                       G_CALLBACK(+[] (GtkRange *range, gpointer data) {
                          if (updating_widgets) return;
                          int imol = GPOINTER_TO_INT(data);
                          set_contour_level_in_sigma(imol, gtk_range_get_value(range));
                          for (const auto &r : map_rows) if (r.imol == imol) update_map_readout(r);
                          graphics_draw();
                       }), GINT_TO_POINTER(imol));
      g_signal_connect(shown, "toggled",
                       G_CALLBACK(+[] (GtkCheckButton *b, gpointer data) {
                          if (updating_widgets) return;
                          set_map_displayed(GPOINTER_TO_INT(data), gtk_check_button_get_active(b));
                       }), GINT_TO_POINTER(imol));

      gtk_box_append(GTK_BOX(maps_box), top);
      gtk_box_append(GTK_BOX(maps_box), scale);
      gtk_box_append(GTK_BOX(maps_box), readout);
   }
   if (n_maps == 0) {
      GtkWidget *l = gtk_label_new("No maps open\n(Load > Auto Open MTZ)");
      gtk_label_set_xalign(GTK_LABEL(l), 0.0);
      gtk_widget_add_css_class(l, "dim-label");
      gtk_box_append(GTK_BOX(maps_box), l);
   }
}

void update_maps_section() {
   std::string sig;
   for (int imol=0; imol<graphics_n_molecules(); imol++)
      if (is_valid_map_molecule(imol))
         sig += std::to_string(imol) + ":" + molecule_name(imol) + ":" + std::to_string(map_is_difference_map(imol)) + ";";
   if (sig != maps_signature) {
      maps_signature = sig;
      updating_widgets = true;
      rebuild_maps_section();
      updating_widgets = false;
      return;
   }
   // follow changes made elsewhere (scroll wheel, +/- keys, Map Parameters dialog)
   updating_widgets = true;
   for (const auto &r : map_rows) {
      double s = get_contour_level_in_sigma(r.imol);
      if (std::abs(gtk_range_get_value(GTK_RANGE(r.scale)) - s) > 0.001)
         gtk_range_set_value(GTK_RANGE(r.scale), s);
      update_map_readout(r);
      bool d = map_is_displayed(r.imol);
      if (gtk_check_button_get_active(GTK_CHECK_BUTTON(r.shown)) != d)
         gtk_check_button_set_active(GTK_CHECK_BUTTON(r.shown), d);
   }
   updating_widgets = false;
}

void update_misc_section() {
   updating_widgets = true;
   double mr = get_map_radius();
   if (std::abs(gtk_range_get_value(GTK_RANGE(map_radius_scale)) - mr) > 0.01)
      gtk_range_set_value(GTK_RANGE(map_radius_scale), mr);
   gtk_label_set_text(GTK_LABEL(map_radius_label), ("Map radius  " + format_float(mr, 0) + " Å").c_str());

   bool symm = symmetry_is_on();
   for (GtkWidget *sw : {symmetry_switch_sidebar, symmetry_switch_inspector})
      if (sw && gtk_switch_get_active(GTK_SWITCH(sw)) != symm)
         gtk_switch_set_active(GTK_SWITCH(sw), symm);
   double sr = graphics_info_t::symmetry_search_radius;
   if (std::abs(gtk_range_get_value(GTK_RANGE(symm_radius_scale)) - sr) > 0.01)
      gtk_range_set_value(GTK_RANGE(symm_radius_scale), sr);
   gtk_label_set_text(GTK_LABEL(symm_radius_label), ("Radius  " + format_float(sr, 0) + " Å").c_str());

   bool any_cell = false;
   for (int imol=0; imol<graphics_n_molecules(); imol++)
      if (is_valid_model_molecule(imol) && get_show_unit_cell(imol)) any_cell = true;
   if (gtk_check_button_get_active(GTK_CHECK_BUTTON(unit_cell_check)) != any_cell)
      gtk_check_button_set_active(GTK_CHECK_BUTTON(unit_cell_check), any_cell);

   static const char *size_names[] = {"", "small", "medium", "large", "larger", "largest"};
   int fs = get_font_size();
   std::string fsn = (fs >= 1 && fs <= 5) ? size_names[fs] : std::to_string(fs);
   gtk_label_set_text(GTK_LABEL(label_size_label), ("Label size  " + fsn).c_str());
   updating_widgets = false;
}

gboolean inspector_tick(gpointer) {
   if (!inspector) return G_SOURCE_REMOVE;
   if (!gtk_widget_get_mapped(inspector)) return G_SOURCE_CONTINUE;
   update_selection_section();
   update_maps_section();
   update_misc_section();
   sync_scroll_buttons();
   return G_SOURCE_CONTINUE;
}

GtkWidget *make_inspector() {

   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
   gtk_widget_add_css_class(box, "coot-plus-inspector");
   gtk_widget_set_margin_start(box, 10);
   gtk_widget_set_margin_end(box, 10);
   gtk_widget_set_margin_bottom(box, 10);

   // --- Selected
   gtk_box_append(GTK_BOX(box), section_heading("Selected"));
   sel_title_label = gtk_label_new("Nothing selected");
   gtk_label_set_xalign(GTK_LABEL(sel_title_label), 0.0);
   gtk_widget_add_css_class(sel_title_label, "title-4");
   gtk_label_set_selectable(GTK_LABEL(sel_title_label), FALSE);
   sel_detail_label = gtk_label_new("");
   gtk_label_set_xalign(GTK_LABEL(sel_detail_label), 0.0);
   gtk_label_set_wrap(GTK_LABEL(sel_detail_label), TRUE);
   gtk_label_set_max_width_chars(GTK_LABEL(sel_detail_label), 30);
   gtk_widget_add_css_class(sel_detail_label, "caption");
   gtk_box_append(GTK_BOX(box), sel_title_label);
   gtk_box_append(GTK_BOX(box), sel_detail_label);
   {
      GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
      const std::vector<std::pair<std::string, std::string> > selected_tools = {
         {"app.residue_info_action", "Info"}, {"kb:Label/Unlabel Active Atom", "Label"}, {"kb:Sphere Refine", "Refine"}};
      for (const auto &t : selected_tools) {
         const command_t *c = resolve_tool(t.first);
         if (!c) continue;
         GtkWidget *b = make_tool_button(*c, t.second, false);
         gtk_widget_set_hexpand(b, TRUE);
         gtk_widget_remove_css_class(b, "flat");
         gtk_box_append(GTK_BOX(row), b);
      }
      gtk_box_append(GTK_BOX(box), row);
   }

   // --- Maps
   gtk_box_append(GTK_BOX(box), section_heading("Maps"));
   maps_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
   gtk_box_append(GTK_BOX(box), maps_box);
   map_radius_label = gtk_label_new("Map radius");
   gtk_label_set_xalign(GTK_LABEL(map_radius_label), 0.0);
   gtk_widget_set_margin_top(map_radius_label, 6);
   map_radius_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 5.0, 60.0, 1.0);
   gtk_scale_set_draw_value(GTK_SCALE(map_radius_scale), FALSE);
   gtk_widget_set_focusable(map_radius_scale, FALSE);
   gtk_widget_set_tooltip_text(map_radius_scale, "How far around the centre the maps are drawn ([ and ] keys)");
   g_signal_connect(map_radius_scale, "value-changed",
                    G_CALLBACK(+[] (GtkRange *range, gpointer) {
                       if (updating_widgets) return;
                       set_map_radius(gtk_range_get_value(range));
                    }), nullptr);
   gtk_box_append(GTK_BOX(box), map_radius_label);
   gtk_box_append(GTK_BOX(box), map_radius_scale);

   // --- Symmetry
   gtk_box_append(GTK_BOX(box), section_heading("Symmetry"));
   gtk_box_append(GTK_BOX(box), make_symmetry_switch_row(&symmetry_switch_inspector));
   symm_radius_label = gtk_label_new("Radius");
   gtk_label_set_xalign(GTK_LABEL(symm_radius_label), 0.0);
   symm_radius_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 5.0, 60.0, 1.0);
   gtk_scale_set_draw_value(GTK_SCALE(symm_radius_scale), FALSE);
   gtk_widget_set_focusable(symm_radius_scale, FALSE);
   gtk_widget_set_tooltip_text(symm_radius_scale, "Show symmetry atoms within this distance of the centre");
   g_signal_connect(symm_radius_scale, "value-changed",
                    G_CALLBACK(+[] (GtkRange *range, gpointer) {
                       if (updating_widgets) return;
                       set_symmetry_size(gtk_range_get_value(range));
                       graphics_draw();
                    }), nullptr);
   gtk_box_append(GTK_BOX(box), symm_radius_label);
   gtk_box_append(GTK_BOX(box), symm_radius_scale);
   unit_cell_check = gtk_check_button_new_with_label("Unit cell");
   gtk_widget_set_focusable(unit_cell_check, FALSE);
   g_signal_connect(unit_cell_check, "toggled",
                    G_CALLBACK(+[] (GtkCheckButton *b, gpointer) {
                       if (updating_widgets) return;
                       set_show_unit_cells_all(gtk_check_button_get_active(b));
                    }), nullptr);
   gtk_box_append(GTK_BOX(box), unit_cell_check);

   // --- Labels
   gtk_box_append(GTK_BOX(box), section_heading("Labels"));
   {
      GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
      label_size_label = gtk_label_new("Label size");
      gtk_label_set_xalign(GTK_LABEL(label_size_label), 0.0);
      gtk_widget_set_hexpand(label_size_label, TRUE);
      GtkWidget *smaller = gtk_button_new_with_label("A−");
      GtkWidget *bigger  = gtk_button_new_with_label("A+");
      gtk_widget_set_focusable(smaller, FALSE);
      gtk_widget_set_focusable(bigger,  FALSE);
      gtk_widget_set_tooltip_text(smaller, "Smaller atom labels");
      gtk_widget_set_tooltip_text(bigger,  "Bigger atom labels");
      auto step = +[] (GtkButton *, gpointer d) { step_label_size(GPOINTER_TO_INT(d)); };
      g_signal_connect(smaller, "clicked", G_CALLBACK(step), GINT_TO_POINTER(-1));
      g_signal_connect(bigger,  "clicked", G_CALLBACK(step), GINT_TO_POINTER(+1));
      gtk_box_append(GTK_BOX(row), label_size_label);
      gtk_box_append(GTK_BOX(row), smaller);
      gtk_box_append(GTK_BOX(row), bigger);
      gtk_box_append(GTK_BOX(box), row);
      const command_t *c = resolve_tool("app.clear_atom_labels_action");
      if (c) gtk_box_append(GTK_BOX(box), make_tool_button(*c));
   }

   GtkWidget *scrolled = gtk_scrolled_window_new();
   gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), box);
   gtk_widget_set_size_request(scrolled, 250, -1);
   gtk_widget_add_css_class(scrolled, "coot-plus-inspector-scroll");

   inspector = scrolled;
   rebuild_maps_section();
   g_timeout_add(400, inspector_tick, nullptr);
   return scrolled;
}

// ------------------------------------------------------------------------------------

GtkWidget *make_panel_toggle(const char *label, const char *tooltip, GtkWidget *panel) {
   GtkWidget *b = gtk_toggle_button_new_with_label(label);
   gtk_widget_set_focusable(b, FALSE);
   gtk_widget_set_tooltip_text(b, tooltip);
   gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), TRUE);
   g_signal_connect(b, "toggled",
                    G_CALLBACK(+[] (GtkToggleButton *t, gpointer p) {
                       gtk_widget_set_visible(GTK_WIDGET(p), gtk_toggle_button_get_active(t));
                    }), panel);
   return b;
}

void load_coot_plus_css() {
   const char *css =
      ".coot-plus-sidebar { border-right: 1px solid alpha(currentColor, 0.15); }\n"
      ".coot-plus-inspector-scroll { border-left: 1px solid alpha(currentColor, 0.15); }\n"
      ".coot-plus-shortcut { font-family: monospace; font-size: smaller; }\n"
      ".coot-plus-sidebar button { padding-top: 3px; padding-bottom: 3px; }\n";
   GtkCssProvider *provider = gtk_css_provider_new();
   gtk_css_provider_load_from_string(provider, css);
   gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

} // namespace


void coot_plus_show_command_search() {
   if (!search_entry) return;
   gtk_widget_grab_focus(search_entry);
   show_search_results();
}


void coot_plus_setup_ui(GtkApplication *application) {

   coot_plus_app = application;
   build_command_registry(application);
   g_info("coot-plus: %zu commands registered", commands.size());

   load_coot_plus_css();

   GtkWidget *main_hbox     = widget_from_builder("main_window_hbox");
   GtkWidget *toolbar_outer = widget_from_builder("main_window_toolbar_hbox_outer");
   GtkWidget *toolbar_left  = widget_from_builder("main_window_toolbar_hbox");
   GtkWidget *graphics      = widget_from_builder("main_window_graphics_overlay");
   if (!main_hbox || !toolbar_outer || !toolbar_left || !graphics) {
      g_warning("coot-plus: main window widgets not found - task interface not added");
      return;
   }

   GtkWidget *side = make_task_sidebar();
   sidebar_panel = side;
   gtk_box_prepend(GTK_BOX(main_hbox), side);
   gtk_widget_set_hexpand(graphics, TRUE);
   GtkWidget *insp = make_inspector();
   inspector_panel = insp;
   gtk_box_insert_child_after(GTK_BOX(main_hbox), insp, graphics);

   GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
   gtk_widget_set_hexpand(search_box, TRUE);
   gtk_widget_set_halign(search_box, GTK_ALIGN_CENTER);
   gtk_box_append(GTK_BOX(search_box), make_panel_toggle("Tasks", "Show/hide the task sidebar", side));
   gtk_box_append(GTK_BOX(search_box), make_search_entry());
   gtk_box_append(GTK_BOX(search_box), make_panel_toggle("Inspector", "Show/hide the inspector", insp));
   graphics_info_t::scroll_wheel_zooms = load_coot_plus_setting_bool("scroll-wheel-zooms", false);
   gtk_box_append(GTK_BOX(search_box), make_scroll_mode_switch());
   gtk_box_insert_child_after(GTK_BOX(toolbar_outer), search_box, toolbar_left);

   GtkWidget *window = graphics_info_t::get_main_window();
   if (window) add_command_search_shortcut(window);

   // the menu item: About > Search Commands...
   GSimpleAction *a = g_simple_action_new("command_search_action", NULL);
   g_signal_connect(a, "activate", G_CALLBACK(+[] (GSimpleAction *, GVariant *, gpointer) {
                                                 coot_plus_show_command_search();
                                              }), nullptr);
   g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(a));
}
