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
#include "c-interface-generic-objects.h"
#include "generic-display-objects-c.h"
#include "cc-interface.hh"
#include "widget-from-builder.hh"
#include "coot-plus-ui.hh"
#include "coot-utils/json.hpp"
#include "session-info.hh"
#include "read-molecule.hh"
#include <filesystem>
#include <fstream>
#include "c-interface-preferences.h"
#include "coot-preferences.h"
#include "utils/logging.hh"
extern logging logger;

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
   std::string description; // what it does (hover text and search results) - verified in the source
   std::string icon;        // icon name (Coot pixmaps), may be empty
   std::string toolbar_id;  // builder id, for toolbar buttons
   std::function<void()> run;
};

const std::map<std::string, std::string> &descriptions(); // key: "app.action", "tb:builder-id", "kb:Description"
const std::map<std::string, std::string> &action_icons();

GtkApplication *coot_plus_app = nullptr;
std::vector<command_t> commands;
std::map<std::string, int> use_count; // key: label + path

bool updating_widgets = false; // guard: don't act on our own set_active()/set_value()

GtkWidget *classic_toolbar = nullptr; // Coot's original vertical build toolbar
void set_classic_toolbar_visible(bool state);

void update_molecules_section(); // inspector Molecules card
void show_find_ligands_window();
void show_assembly_window();
void set_custom_label(int imol, const std::string &label);
void set_symmetry_view(bool state);
void show_blobs_window();
void show_problems_window();
void show_problems_on_model(bool on);
GtkWidget *phenix_box = nullptr;
void refresh_phenix_box();
void choose_phenix_folder();
void update_sequence_strip();
void update_ligand_card();
void step_residue(int delta);
void ask_rename_molecule(int imol);
std::string config_path(const std::string &file);
nlohmann::json read_json_file(const std::string &fn, const nlohmann::json &dflt);
void write_json_file(const std::string &fn, const nlohmann::json &j);
std::string display_name(int imol);
void undo_view_change();
void clear_drawn_objects();
void log_line(const std::string &line);
// Coot records calls in its history - including the getters Coot+ polls. History made
// while one of our timers runs is not the user's, so it is skipped by the command log.
std::vector<std::pair<size_t, size_t> > history_ignored; // [begin, end)
struct ignore_history_scope {
   size_t begin;
   ignore_history_scope() : begin(graphics_info_t::history_list.history_strings.size()) {}
   ~ignore_history_scope() {
      size_t end = graphics_info_t::history_list.history_strings.size();
      if (end > begin) history_ignored.push_back(std::make_pair(begin, end));
   }
};
void show_command_log();
GtkWidget *recent_box = nullptr;
GtkWidget *groups_box = nullptr;
void refresh_recent_and_groups();
std::vector<GtkWidget *> sidebar_shortcut_labels, sidebar_hint_labels, sidebar_tool_labels;
GtkWidget *sidebar_help_box = nullptr;
bool compact_sidebar = false;
void apply_compact_sidebar(bool on);
void set_compact_sidebar(bool on);
void show_python_console();
void add_model_look_entries(GtkWidget *box, GtkWidget *pop, int imol);
void add_map_look_entries(GtkWidget *box, GtkWidget *pop, int imol);
GtkWidget *make_colour_swatch(int imol, bool model);
std::vector<command_t> residue_search_results(const std::string &query);
void save_open_files_as_group(const std::string &name);
void ask_name(const std::string &title, const std::string &prompt, const std::string &initial,
              std::function<void(const std::string &)> cb, bool allow_empty = false);
void cancel_pick_modes();
void load_favourites();
void rebuild_favourites();
std::vector<std::string> favourites; // command keys (favourite tools)
GtkWidget *favourites_box = nullptr;

struct preset_t {
   std::string name, task, description;
   bool symmetry = false;
   double symm_radius = 0, map_radius = 0;
   int font_size = 0;              // 0: leave
   int panels = -1;                // -1 leave, 0 hide both, 1 show both
   std::string coot_action;        // e.g. scene_preset_figure_making_action
   bool problems = false;
};

std::vector<preset_t> builtin_presets();
void apply_preset(const preset_t &p);
void set_theme(const std::string &theme);
std::string building_task; // the task whose page is being built (for right-click menus)
std::vector<std::string> custom_common(const std::string &task);
void save_custom_common(const std::string &task, const std::vector<std::string> &keys, bool remove);
std::string command_key(const command_t &c);
std::string load_coot_plus_setting_string(const char *key, const std::string &dflt);
void rebuild_sidebar();
GtkWidget *make_script_page();
GtkWidget *console_notebook = nullptr;
GtkWidget *make_snippets_page();
const std::vector<std::pair<std::string, std::string> > &accent_colours();
void set_accent(const std::string &hex);
struct common_menu_t { std::string task; std::string key; std::string entry; std::vector<std::string> cur; bool in = false; GtkWidget *pop = nullptr; };

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
      {"decreasecontourlevel", 15}, {"undoview", 40}, {"pythonconsole", 45}, {"cleardrawnobjectsdotsoverlapsmarkers", 35}, {"scrollwheelzooms", 40}, {"togglescrollwheelzoomcontour", 30}};
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
      {"vertical_toolbar_add_terminal_residue_button", "Add Terminal Residue"},
      {"add_alt_conf_button",                       "Add Alt Conf"},
      {"undo_button",                               "Undo"},
      {"redo_button",                               "Redo"},
      {"toolbar_reset_view_button",                 "Reset View"},
      {"toolbar_display_manager_button",            "Display Manager"},
      {"go_to_ligand_button",                       "Go to Next Ligand"},
      {"graphics-grab-focus-button",                "Grab Keyboard Focus"},
      {"python_scripting_button",                   "Python Command Line"}};
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
      c.toolbar_id = tb.first;
      for (GtkWidget *ch = w; ch; ) { // the button's icon
         if (GTK_IS_IMAGE(ch)) {
            const char *icon_name = gtk_image_get_icon_name(GTK_IMAGE(ch));
            if (icon_name) c.icon = icon_name;
            break;
         }
         GtkWidget *next = gtk_widget_get_first_child(ch);
         if (!next) {
            while (ch && ch != w && !gtk_widget_get_next_sibling(ch)) ch = gtk_widget_get_parent(ch);
            next = (ch && ch != w) ? gtk_widget_get_next_sibling(ch) : nullptr;
         }
         ch = next;
      }
      std::string id = tb.first;
      c.run = [id] () {
         GtkWidget *b = widget_from_builder(id);
         if (!b) return;
         // not gtk_widget_activate(): GtkButton ignores that until it has been shown,
         // and the classic toolbar is usually hidden
         if (GTK_IS_BUTTON(b)) g_signal_emit_by_name(b, "clicked");
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

   for (auto &c : commands) {
      std::string key = !c.toolbar_id.empty() ? "tb:" + c.toolbar_id
                      : (!c.action.empty() ? c.action : "kb:" + c.label);
      auto it = descriptions().find(key);
      if (it == descriptions().end() && c.path == "Keyboard")
         it = descriptions().find("kb:" + to_lower(c.label));
      if (it != descriptions().end()) c.description = it->second;
      if (c.icon.empty() && !c.action.empty()) {
         auto it_i = action_icons().find(c.action);
         if (it_i != action_icons().end()) c.icon = it_i->second;
      }
      if (c.icon.empty() && c.label == "Add Water Here") c.icon = "coot-plus-tool-add-water-symbolic";
      if (c.icon.empty() && c.label == "Increase Map Radius") c.icon = "coot-plus-tool-radius-larger-symbolic";
      if (c.icon.empty() && c.label == "Decrease Map Radius") c.icon = "coot-plus-tool-radius-smaller-symbolic";
      if (c.icon.empty() && c.label == "Show Go To Residue Keyboarding Window") c.icon = "coot-plus-tool-type-residue-symbolic";
   }
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
   log_line("# " + (c.path.empty() ? std::string() : c.path + " > ") + c.label);

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
std::vector<command_t> search_results; // in row order

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

   if (!c.description.empty()) {
      GtkWidget *d = gtk_label_new(c.description.c_str());
      gtk_label_set_xalign(GTK_LABEL(d), 0.0);
      gtk_label_set_wrap(GTK_LABEL(d), TRUE);
      gtk_label_set_max_width_chars(GTK_LABEL(d), 60);
      gtk_widget_add_css_class(d, "caption");
      gtk_box_append(GTK_BOX(row_box), d);
   }
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
   if (!query.empty())
      for (const auto &c : residue_search_results(gtk_editable_get_text(GTK_EDITABLE(search_entry)))) {
         search_results.push_back(c);
         gtk_list_box_append(GTK_LIST_BOX(search_listbox), make_result_row(c));
      }
   for (unsigned int i=0; i<scored.size() && search_results.size()<max_rows; i++) {
      search_results.push_back(commands[scored[i].second]);
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
   command_t c = search_results[row_index]; // copy: running may rebuild things
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
   add("Undo View", "Header", "undo display view dots overlaps surface ribbons colours contour symmetry labels revert",
       [] () { undo_view_change(); });
   add("Clear Drawn Objects (dots, overlaps, markers)", "Validate", "clear remove dots overlaps contact spheres markers generic objects colours",
       [] () { clear_drawn_objects(); });
   add("Python Console", "Header", "python console terminal shell scripting command line repl code",
       [] () { show_python_console(); });
   add("Compact / Full Sidebar", "Panels", "sidebar narrow compact small screen width",
       [] () { set_compact_sidebar(!compact_sidebar); });
   add("Save Open Files as a Group", "Load > Groups", "group project session save files together",
       [] () { ask_name("Save Group", "Name for this group of files:", "My project",
                        [] (const std::string &n) { save_open_files_as_group(n); }); });
   add("Previous Residue", "Inspector", "step back previous residue chain walk", [] () { step_residue(-1); });
   add("Next Residue", "Inspector", "step forward next residue chain walk", [] () { step_residue(+1); });
   add("Unmodelled Blobs List", "Validate", "blobs density unexplained ligand missing", [] () { show_blobs_window(); });
   add("Problems List", "Validate", "outliers problems validation ramachandran rotamer density fit", [] () { show_problems_window(); });
   add("Show Problems on the Model", "Validate", "validation markers flags outliers on model", [] () { show_problems_on_model(true); });
   add("Remove Problem Markers", "Validate", "validation markers clear", [] () { show_problems_on_model(false); });
   add("Open Phenix Refinement", "Load > Phenix", "phenix refine run folder open latest newest", [] () { choose_phenix_folder(); });
   add("Find Ligands (Coot+)", "Ligands", "ligand fit dock blob find place", [] () { show_find_ligands_window(); });
   add("Make Assembly from Symmetry", "Symmetry", "assembly trimer dimer oligomer symmetry copies chains write biological unit",
       [] () { show_assembly_window(); });
   for (const auto &p : builtin_presets())
      add("Workspace: " + p.name, "Header > Workspace", "workspace preset layout setup " + to_lower(p.name), [p] () { apply_preset(p); });
   add("Light Theme", "Preferences > Coot+", "light theme white appearance", [] () { set_theme("light"); });
   add("Dark Theme", "Preferences > Coot+", "dark theme black appearance", [] () { set_theme("dark"); });
   add("Command Log", "Header > More", "history log record replay script python macro commands",
       [] () { show_command_log(); });
   add("Cancel Pick Mode (Esc)", "Banner", "cancel stop pick click mode escape",
       [] () { cancel_pick_modes(); });
   add("Show/Hide Classic Build Toolbar", "Build", "classic toolbar old buttons right column vertical",
       [] () { if (classic_toolbar) set_classic_toolbar_visible(!gtk_widget_get_visible(classic_toolbar)); });
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
   std::string icon;
   std::string hint;
   // entries:
   //   "app.action[=target][|Label]"    a command (optional shorter label)
   //   "kb:Description[|Label]"         a key binding
   //   "tb:Toolbar Label[|Label]"       a toolbar button
   //   "menu:builder-menu-id|Label|icon" a button that opens one of the toolbar menus
   //   "section:Title"                  a heading
   //   "special:..."                    a custom widget
   std::vector<std::string> tools;
   std::vector<std::string> common; // shown first; "tools" (the full list) goes in an expander
};

// the few tools most people use, per task
const std::map<std::string, std::vector<std::string> > &common_tools() {
   static const std::map<std::string, std::vector<std::string> > m = {
      {"Load",     {"section:Open", "app.open_coordinates_action", "app.auto_open_mtz_action", "app.open_map_action",
                    "app.show_accession_code_fetch_frame_oca|PDB Entry", "app.get_monomer_action",
                    "section:Phenix", "special:phenix",
                    "section:Groups", "special:groups", "section:Recent files", "special:recent"}},
      {"Navigate", {"app.go_to_atom_action", "kb:Show Go To Residue Keyboarding Window|Type a Residue Number",
                    "tb:Go to Next Ligand", "tb:Reset View", "tb:Display Manager"}},
      {"Maps",     {"special:maps-hint", "app.map_parameters_action", "kb:Increase Map Radius",
                    "kb:Decrease Map Radius", "app.make_a_difference_map_action"}},
      {"Symmetry", {"special:symmetry-toggle", "special:assembly", "app.draw_cell_and_symmetry_action|Symmetry Settings",
                    "app.undo_symmetry_view_action", "app.save_symmetry_coordinates_action"}},
      {"Build",    {"special:range-pair", "app.refine_sphere|Refine Sphere", "app.refine_range|Refine Range",
                    "tb:Auto-Fit Rotamer", "tb:Flip Peptide", "menu:mutate-protein-menu|Mutate & Auto-Fit|mutate-auto-fit",
                    "kb:Add Water Here", "app.delete_item_residue|Delete Residue"}},
      {"Ligands",  {"app.get_monomer_action", "special:find-ligands", "special:blobs",
                    "app.jiggle_fit_active_residue_action", "app.geometric_distortions_for_ligand_action"}},
      {"Validate", {"special:validation-markers", "special:problems-list", "special:blobs",
                    "app.show_validation_graphs_dialog|Validation Graphs", "app.difference_map_peaks_action",
                    "app.ramachandran_plot_action", "special:clear-drawn"}},
      {"Save",     {"app.save_coordinates_action", "app.save_state_action", "app.screenshot_action"}}};
   return m;
}

const std::vector<task_t> &tasks() {
   static const std::vector<task_t> v = {
      {"Load", "coot-plus-load-symbolic", "Open models, data and maps", {
            "section:Open",
            "app.open_coordinates_action", "app.auto_open_mtz_action", "app.open_dataset_action|Open Reflection Data",
            "app.open_map_action",
            "section:Fetch from the web",
            "app.show_accession_code_fetch_frame_oca|PDB Entry", "app.show_accession_code_fetch_frame_eds|PDB Entry + Map (EDS)",
            "app.show_accession_code_fetch_frame_pdb_redo|PDB-REDO Model + Map",
            "app.show_accession_code_fetch_frame_uniprot_id|AlphaFold Model",
            "section:Ligand dictionaries",
            "app.get_monomer_action", "app.import_cif_dictionary_action",
            "section:Session",
            "app.recover_session_action", "app.load_tutorial_model_and_data_action|Tutorial Model + Data"}},
      {"Navigate", "coot-plus-navigate-symbolic", "Move around the model", {
            "section:Go to",
            "app.go_to_atom_action", "kb:Show Go To Residue Keyboarding Window|Type a Residue Number",
            "tb:Go to Next Ligand",
            "section:View",
            "tb:Reset View", "app.undo_last_navigation_action", "app.display_only_active_action",
            "section:Molecules",
            "tb:Display Manager", "app.sequence_view_action", "app.residue_type_selection_action"}},
      {"Maps", "coot-plus-maps-symbolic", "Contour, radius and map tools", {
            "special:maps-hint",
            "section:Contour and radius",
            "app.map_parameters_action", "kb:Increase Map Radius", "kb:Decrease Map Radius",
            "kb:Display Next Map", "kb:Change Scroll-wheel map",
            "section:Make maps",
            "app.make_a_difference_map_action", "app.sharpen_blur_for_xray_action|Sharpen / Blur",
            "app.another_contour_level_action", "app.mask_map_by_atom_selection_action",
            "app.set_map_is_difference_map_action"}},
      {"Symmetry", "coot-plus-symmetry-symbolic", "Crystal neighbours and unit cell", {
            "special:symmetry-toggle",
            "section:Tools",
            "app.draw_cell_and_symmetry_action|Symmetry Settings", "app.undo_symmetry_view_action",
            "app.all_molecules_near_chains_symmetry_action|Whole Chains for All Models",
            "app.save_symmetry_coordinates_action", "app.symm_shift_reference_chain_here_action"}},
      {"Build", "coot-plus-build-symbolic", "Fit and fix the model", {
            "section:Pick a range",
            "special:range-pair",
            "section:Refine",
            "app.refine_sphere|Refine Sphere", "app.refine_range|Refine Range", "app.refine_chain|Refine Chain",
            "app.refine_regularize_sphere|Regularize Sphere", "menu:refine-menu|More Refine Options|refine-1",
            "tb:Refine/Regularize Control|Refinement Settings", "tb:Select Map for Fitting|Map for Refinement",
            "menu:fixed-atoms-menu|Fixed Atoms|anchor",
            "section:Side chains",
            "tb:Auto-Fit Rotamer", "tb:Rotamers|Choose Rotamer", "tb:Edit Chi Angles",
            "tb:Sidechain 180° Flip", "app.fill_partial_residues_action",
            "section:Backbone",
            "tb:Flip Peptide", "tb:Add Terminal Residue",
            "section:Mutate",
            "menu:mutate-protein-menu|Mutate & Auto-Fit|mutate-auto-fit", "tb:Simple Mutate",
            "section:Move",
            "app.rigid_body_fit_residue_range_action|Rigid-Body Fit Range",
            "app.rigid_body_fit_chain_action|Rigid-Body Fit Chain",
            "app.rotate_translate_residue_range|Rotate/Translate Range",
            "app.rotate_translate_chain|Rotate/Translate Chain",
            "menu:rigid-body-fit-menu|More Rigid-Body Options|rigid-body",
            "menu:rotate-translate-menu|More Rotate/Translate Options|rtz",
            "section:Add",
            "kb:Add Water Here", "tb:Add Alt Conf", "app.add_an_atom_action",
            "section:Delete",
            "app.delete_item_residue|Delete Residue", "app.delete_item_side_chain|Delete Side Chain",
            "app.delete_item_atom|Delete Atom", "app.delete_item_water|Delete Water",
            "app.delete_item_residue_range|Delete Residue Range", "menu:delete-item-menu|More Delete Options|delete",
            "section:History",
            "tb:Undo", "tb:Redo",
            "special:classic-toolbar"}},
      {"Ligands", "coot-plus-ligands-symbolic", "Find, fit and check ligands", {
            "section:Get a ligand",
            "app.get_monomer_action", "app.fetch_pdbe_ligand_description_action",
            "app.fetch_ligand_restraints_from_github_action", "app.import_cif_dictionary_action",
            "app.SMILES_to_3D_action",
            "section:Fit",
            "app.find_ligands_action", "app.unmodelled_blobs_action", "tb:Go to Next Ligand",
            "app.jiggle_fit_active_residue_action",
            "section:Check",
            "app.geometric_distortions_for_ligand_action", "app.coot_contact_dots_for_ligand_action",
            "app.flev_action",
            "section:Build and compare",
            "app.ligand_builder_action", "app.superpose_ligands_action"}},
      {"Validate", "coot-plus-validate-symbolic", "Find what needs fixing", {
            "section:Overview",
            "app.show_validation_graphs_dialog|Validation Graphs", "app.validation_outliers_action",
            "app.ramachandran_plot_action",
            "app.overlaps_peptides_cbeta_ramas_and_rotas_action|All Outliers (List)",
            "section:Density",
            "app.difference_map_peaks_action", "app.unmodelled_blobs_action",
            "app.pepflips_from_difference_map_action",
            "section:Contacts and waters",
            "app.atoms_overlaps_action", "app.all_atom_contact_dots_molprobity_action|Contact Dots",
            "special:clear-drawn",
            "app.check_delete_waters_action"}},
      {"Save", "coot-plus-save-symbolic", "Write models, maps and pictures", {
            "section:Model",
            "app.save_coordinates_action", "app.save_symmetry_coordinates_action",
            "section:Maps",
            "app.file_export_map_action", "app.file_export_map_fragment_action",
            "section:Session and pictures",
            "app.save_state_action", "app.screenshot_action"}}
   };
   return v;
}

// split "ref|Label|icon"
std::vector<std::string> split_bar(const std::string &s) {
   std::vector<std::string> r;
   size_t start = 0;
   while (true) {
      size_t p = s.find('|', start);
      r.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
      if (p == std::string::npos) break;
      start = p + 1;
   }
   return r;
}

const command_t *resolve_tool(const std::string &ref_in) {
   std::string ref = split_bar(ref_in)[0];
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

std::string tooltip_for(const command_t &c) {
   std::string tip;
   if (!c.description.empty()) tip = c.description + "\n";
   tip += c.path.empty() ? c.label : c.path + " › " + c.label;
   if (!c.shortcut.empty()) tip += "   —   key: " + c.shortcut;
   return tip;
}

// Coot's toolbar icons are a mix of clip-art styles; show Coot+'s line icons instead
// (pixmaps/coot-plus-tool-*-symbolic.svg), which also follow the light/dark theme.
std::string modern_icon(const std::string &icon) {
   static const std::map<std::string, std::string> m = {
      {"add-alt-conf", "add-alt-conf"}, {"add-peptide-1", "add-residue"}, {"anchor", "fixed-atoms"},
      {"auto-fit-rotamer", "auto-fit-rotamer"}, {"broom", "clear-dots"}, {"cell+symm", "unit-cell"},
      {"connect-to-world", "download-pdb"}, {"coot-configuration", "settings"}, {"coot-delete", "delete"},
      {"delete", "delete-residue"}, {"density-fit", "problems"}, {"diff-map", "diff-map"},
      {"edit-chi", "edit-chi"}, {"flip-peptide", "flip-peptide"}, {"go-to-atom", "go-to-atom"},
      {"go-to-ligand", "go-to-ligand"}, {"map", "open-map"}, {"mutate", "mutate"},
      {"mutate-auto-fit", "mutate-auto-fit"}, {"python-logo", "python"}, {"range-pair", "range"},
      {"refine-1", "refine"}, {"reset-view", "reset-view"}, {"rigid-body", "rigid-body"},
      {"rotamers", "rotamers"}, {"rtz", "rotate-translate"}, {"side-chain-180", "side-chain-180"},
      {"unknown-blob", "blob"}, {"sequence-view", "sequence"}, {"regularize-1", "regularize"},
      {"layla_6arom", "ligand-builder"}, {"smiles", "smiles"}, {"probe-clash", "contact-dots"},
      {"water-drop", "water"}, {"residue-info", "residue-info"}, {"atom-at-pointer", "add-atom"},
      {"env-distances", "distances"}, {"pointer-distances", "pointer-distance"}};
   if (icon == "coot-undo") return "coot-plus-undo-symbolic";
   if (icon == "coot-redo") return "coot-plus-redo-symbolic";
   if (icon == "display-manager") return "coot-plus-molecules-symbolic";
   auto it = m.find(icon);
   return it == m.end() ? icon : "coot-plus-tool-" + it->second + "-symbolic";
}

GtkWidget *tool_button_content(const std::string &icon_in, const std::string &label, const std::string &shortcut) {
   std::string icon = modern_icon(icon_in);
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
   GtkWidget *img = icon.empty() ? gtk_image_new() : gtk_image_new_from_icon_name(icon.c_str());
   gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
   gtk_widget_set_size_request(img, 16, 16);
   gtk_widget_add_css_class(img, "coot-plus-tool-icon");
   gtk_box_append(GTK_BOX(box), img);
   GtkWidget *l = gtk_label_new(label.c_str());
   gtk_label_set_xalign(GTK_LABEL(l), 0.0);
   gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
   gtk_label_set_max_width_chars(GTK_LABEL(l), compact_sidebar ? 14 : 19);
   gtk_widget_set_hexpand(l, TRUE);
   gtk_box_append(GTK_BOX(box), l);
   sidebar_tool_labels.push_back(l);
   if (!shortcut.empty()) {
      GtkWidget *sc = gtk_label_new(shortcut.c_str());
      gtk_widget_add_css_class(sc, "coot-plus-shortcut");
      gtk_box_append(GTK_BOX(box), sc);
      sidebar_shortcut_labels.push_back(sc);
      if (compact_sidebar) gtk_widget_set_visible(sc, FALSE);
   }
   return box;
}

// the help box at the bottom of the sidebar: describes the tool under the mouse
GtkWidget *help_title = nullptr;
GtkWidget *help_text  = nullptr;
std::string help_default_title, help_default_text;

void show_help(const std::string &title, const std::string &text) {
   if (!help_title) return;
   gtk_label_set_text(GTK_LABEL(help_title), title.c_str());
   gtk_label_set_text(GTK_LABEL(help_text), text.c_str());
}

void show_default_help() { show_help(help_default_title, help_default_text); }

void attach_hover_help(GtkWidget *w, const std::string &title, const std::string &text) {
   GtkEventController *motion = gtk_event_controller_motion_new();
   std::string *t = new std::string(title + "\x1f" + text);
   g_signal_connect_data(motion, "enter",
                         G_CALLBACK(+[] (GtkEventControllerMotion *, double, double, gpointer d) {
                            const std::string &s = *static_cast<std::string *>(d);
                            size_t p = s.find('\x1f');
                            show_help(s.substr(0, p), s.substr(p + 1));
                         }), t, +[] (gpointer d, GClosure *) { delete static_cast<std::string *>(d); }, GConnectFlags(0));
   g_signal_connect(motion, "leave", G_CALLBACK(+[] (GtkEventControllerMotion *, gpointer) { show_default_help(); }), nullptr);
   gtk_widget_add_controller(w, motion);
}

std::string help_text_for(const command_t &c) {
   std::string t = c.description.empty() ? std::string("(no description yet)") : c.description;
   std::string where = c.path.empty() ? std::string() : c.path;
   if (!c.shortcut.empty()) where += (where.empty() ? "" : "   \u00b7   ") + std::string("key ") + c.shortcut;
   if (!where.empty()) t += "\n" + where;
   return t;
}

void attach_favourite_menu(GtkWidget *button, const command_t &c, const std::string &shown_label = ""); // below

GtkWidget *make_tool_button(const command_t &c, const std::string &short_label = "", bool show_shortcut = true) {

   GtkWidget *button = gtk_button_new();
   std::string first_key = c.shortcut.substr(0, c.shortcut.find(','));
   gtk_button_set_child(GTK_BUTTON(button),
                        tool_button_content(c.icon, short_label.empty() ? c.label : short_label,
                                            show_shortcut ? first_key : std::string()));
   gtk_widget_add_css_class(button, "flat");
   gtk_widget_add_css_class(button, "coot-plus-tool");
   gtk_widget_set_focusable(button, FALSE); // keep keyboard shortcuts going to the graphics
   gtk_widget_set_tooltip_text(button, tooltip_for(c).c_str());
   attach_hover_help(button, short_label.empty() ? c.label : short_label, help_text_for(c));
   attach_favourite_menu(button, c, short_label.empty() ? c.label : short_label);

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

GtkWidget *make_menu_tool_button(const std::string &menu_id, const std::string &label, const std::string &icon) {
   GObject *o = graphics_info_t::get_gobject_from_builder(menu_id);
   if (!o || !G_IS_MENU_MODEL(o)) return nullptr;
   GtkWidget *mb = gtk_menu_button_new();
   gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), G_MENU_MODEL(o));
   GtkWidget *content = tool_button_content(icon, label, "");
   GtkWidget *arrow = gtk_label_new("▾");
   gtk_widget_add_css_class(arrow, "coot-plus-menu-arrow");
   gtk_box_append(GTK_BOX(content), arrow);
   gtk_menu_button_set_child(GTK_MENU_BUTTON(mb), content);
   gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(mb), FALSE);
   gtk_widget_add_css_class(mb, "coot-plus-tool");
   gtk_widget_set_focusable(mb, FALSE);
   auto it = descriptions().find("menu:" + menu_id);
   if (it != descriptions().end()) {
      gtk_widget_set_tooltip_text(mb, it->second.c_str());
      attach_hover_help(mb, label, it->second + "\nOpens a menu of choices");
   }
   return mb;
}

GtkWidget *make_symmetry_switch_row(GtkWidget **switch_store) {
   GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
   gtk_widget_add_css_class(row, "coot-plus-switch-row");
   GtkWidget *l = gtk_label_new("Show symmetry");
   gtk_label_set_xalign(GTK_LABEL(l), 0.0);
   gtk_widget_set_hexpand(l, TRUE);
   gtk_widget_set_tooltip_text(row, "Show the crystal neighbours (symmetry copies) around the centre, as full atoms.\n"
                                    "Turns on symmetry for every model and sets the radius to 40 Å if it was smaller.");
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

GtkWidget *section_label(const std::string &text) {
   std::string t = text;
   for (auto &ch : t) ch = std::toupper(static_cast<unsigned char>(ch));
   GtkWidget *l = gtk_label_new(t.c_str());
   gtk_label_set_xalign(GTK_LABEL(l), 0.0);
   gtk_widget_add_css_class(l, "coot-plus-section");
   return l;
}

GtkWidget *sidebar_task_list = nullptr;
GtkWidget *sidebar_stack = nullptr;

void populate_sidebar(GtkWidget *outer);

GtkWidget *make_task_sidebar() {
   GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
   gtk_widget_add_css_class(outer, "coot-plus-sidebar");
   gtk_widget_set_size_request(outer, 210, -1);
   populate_sidebar(outer);
   sidebar = outer;
   return outer;
}

std::string current_task_name() {
   if (!sidebar_stack) return "";
   const char *n = gtk_stack_get_visible_child_name(GTK_STACK(sidebar_stack));
   return n ? n : "";
}

void select_task(const std::string &name) {
   if (!sidebar_task_list) return;
   for (int i=0; ; i++) {
      GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(sidebar_task_list), i);
      if (!row) break;
      if (name == gtk_widget_get_name(GTK_WIDGET(row))) { gtk_list_box_select_row(GTK_LIST_BOX(sidebar_task_list), row); break; }
   }
}

void rebuild_sidebar() {
   if (!sidebar) return;
   std::string task = current_task_name();
   GtkWidget *c;
   while ((c = gtk_widget_get_first_child(sidebar)) != nullptr) gtk_box_remove(GTK_BOX(sidebar), c);
   sidebar_shortcut_labels.clear(); sidebar_hint_labels.clear(); sidebar_tool_labels.clear();
   symmetry_switch_sidebar = nullptr; recent_box = nullptr; groups_box = nullptr; phenix_box = nullptr;
   favourites_box = nullptr; help_title = nullptr; help_text = nullptr; sidebar_help_box = nullptr;
   populate_sidebar(sidebar);
   apply_compact_sidebar(compact_sidebar);
   refresh_recent_and_groups();
   if (!task.empty()) select_task(task);
}

void populate_sidebar(GtkWidget *outer) {

   GtkWidget *stack = gtk_stack_new();
   gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
   gtk_stack_set_transition_duration(GTK_STACK(stack), 120);
   gtk_stack_set_hhomogeneous(GTK_STACK(stack), FALSE);

   GtkWidget *task_list = gtk_list_box_new();
   gtk_list_box_set_selection_mode(GTK_LIST_BOX(task_list), GTK_SELECTION_BROWSE);
   gtk_widget_add_css_class(task_list, "coot-plus-task-list");
   gtk_widget_set_focusable(task_list, FALSE);
   sidebar_task_list = task_list;
   sidebar_stack = stack;

   {
      // Favourites: the first task
      GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
      GtkWidget *star = gtk_label_new("\u2605");
      gtk_widget_add_css_class(star, "coot-plus-star");
      gtk_box_append(GTK_BOX(row_box), star);
      GtkWidget *rl = gtk_label_new("Favourites");
      gtk_label_set_xalign(GTK_LABEL(rl), 0.0);
      gtk_box_append(GTK_BOX(row_box), rl);
      GtkWidget *row = gtk_list_box_row_new();
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
      gtk_widget_set_focusable(row, FALSE);
      gtk_widget_set_name(row, "Favourites");
      gtk_widget_set_tooltip_text(row, "Your pinned tools - right-click any tool to add it");
      gtk_list_box_append(GTK_LIST_BOX(task_list), row);
      GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_add_css_class(page, "coot-plus-task-page");
      GtkWidget *title = gtk_label_new("Favourites");
      gtk_label_set_xalign(GTK_LABEL(title), 0.0);
      gtk_widget_add_css_class(title, "coot-plus-page-title");
      gtk_box_append(GTK_BOX(page), title);
      favourites_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_set_margin_top(favourites_box, 6);
      gtk_box_append(GTK_BOX(page), favourites_box);
      GtkWidget *scrolled = gtk_scrolled_window_new();
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
      gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), page);
      gtk_stack_add_named(GTK_STACK(stack), scrolled, "Favourites");
   }

   for (const auto &task : tasks()) {

      GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
      GtkWidget *row_icon = gtk_image_new_from_icon_name(task.icon.c_str());
      gtk_image_set_pixel_size(GTK_IMAGE(row_icon), 16);
      GtkWidget *row_label = gtk_label_new(task.name.c_str());
      gtk_label_set_xalign(GTK_LABEL(row_label), 0.0);
      gtk_box_append(GTK_BOX(row_box), row_icon);
      gtk_box_append(GTK_BOX(row_box), row_label);
      GtkWidget *row = gtk_list_box_row_new();
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
      gtk_widget_set_focusable(row, FALSE);
      gtk_widget_set_name(row, task.name.c_str());
      gtk_widget_set_tooltip_text(row, task.hint.c_str());
      gtk_list_box_append(GTK_LIST_BOX(task_list), row);

      GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_add_css_class(page, "coot-plus-task-page");
      GtkWidget *title = gtk_label_new(task.name.c_str());
      gtk_label_set_xalign(GTK_LABEL(title), 0.0);
      gtk_widget_add_css_class(title, "coot-plus-page-title");
      GtkWidget *hint = gtk_label_new(task.hint.c_str());
      gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
      gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
      gtk_label_set_max_width_chars(GTK_LABEL(hint), 24);
      gtk_widget_add_css_class(hint, "coot-plus-page-hint");
      sidebar_hint_labels.push_back(hint);
      gtk_box_append(GTK_BOX(page), title);
      gtk_box_append(GTK_BOX(page), hint);

      std::set<std::string> added; // the same command only once per list
      // append one entry (see task_t) to box; returns false if it was skipped
      auto append_entry = [&] (GtkWidget *page, const std::string &ref, bool allow_special) {

         if (ref.rfind("section:", 0) == 0) {
            gtk_box_append(GTK_BOX(page), section_label(ref.substr(8)));
            return;
         }
         if (ref == "special:symmetry-toggle") {
            gtk_box_append(GTK_BOX(page), make_symmetry_switch_row(&symmetry_switch_sidebar));
            return;
         }
         if (ref == "special:maps-hint") {
            GtkWidget *l = gtk_label_new("Contour sliders for each map are in the inspector on the right.");
            gtk_label_set_xalign(GTK_LABEL(l), 0.0);
            gtk_label_set_wrap(GTK_LABEL(l), TRUE);
            gtk_label_set_max_width_chars(GTK_LABEL(l), 24);
            gtk_widget_add_css_class(l, "coot-plus-page-hint");
            gtk_box_append(GTK_BOX(page), l);
            return;
         }
         if (ref == "special:phenix") {
            phenix_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            gtk_box_append(GTK_BOX(page), phenix_box);
            refresh_phenix_box();
            return;
         }
         if (ref == "special:validation-markers") {
            GtkWidget *t = gtk_toggle_button_new();
            gtk_button_set_child(GTK_BUTTON(t), tool_button_content("density-fit", "Show Problems on the Model", ""));
            gtk_widget_add_css_class(t, "flat");
            gtk_widget_add_css_class(t, "coot-plus-tool");
            gtk_widget_set_focusable(t, FALSE);
            gtk_widget_set_tooltip_text(t, "Marks residues with problems in the 3D view: red = serious (Ramachandran outlier, "
                                           "rotamer outlier, CC < 0.4), orange = minor. Uses the model at the centre and the "
                                           "refinement map. Click again to remove the markers.");
            attach_hover_help(t, "Show Problems on the Model", "Red markers: Ramachandran or rotamer outliers, or density "
                              "CC below 0.4. Orange: allowed-but-unusual Ramachandran or CC 0.4-0.6.");
            g_signal_connect(t, "toggled", G_CALLBACK(+[] (GtkToggleButton *b, gpointer) {
                                show_problems_on_model(gtk_toggle_button_get_active(b)); }), nullptr);
            gtk_box_append(GTK_BOX(page), t);
            return;
         }
         if (ref == "special:find-ligands" || ref == "special:assembly") {
            bool fl = ref == "special:find-ligands";
            command_t c;
            c.label = fl ? "Find Ligands" : "Make Assembly from Symmetry";
            c.icon = fl ? "go-to-ligand" : "cell+symm";
            c.description = fl ? "Fits a loaded ligand into unexplained density - right here at the centre, or the whole map. "
                                 "The 2mFo-DFc map and the protein are picked for you."
                               : "Finds the symmetry copies that touch the model and makes a new model with them as extra "
                                 "chains (e.g. a trimer as chains A, B, C).";
            c.run = fl ? std::function<void()>([] () { show_find_ligands_window(); })
                       : std::function<void()>([] () { show_assembly_window(); });
            gtk_box_append(GTK_BOX(page), make_tool_button(c));
            return;
         }
         if (ref == "special:problems-list" || ref == "special:blobs") {
            bool blobs = ref == "special:blobs";
            command_t c;
            c.label = blobs ? "Unmodelled Blobs List" : "Problems List";
            c.icon = blobs ? "unknown-blob" : "coot-plus-tool-outliers-symbolic";
            c.description = blobs ? "Lists density the model doesn't explain (too big for a water), biggest first, with the "
                                    "nearest residues. Click one to go there."
                                  : "Lists residues with Ramachandran, rotamer or density-fit problems, worst first. "
                                    "Click one to go there.";
            c.run = blobs ? std::function<void()>([] () { show_blobs_window(); }) : std::function<void()>([] () { show_problems_window(); });
            gtk_box_append(GTK_BOX(page), make_tool_button(c));
            return;
         }
         if (ref == "special:recent" || ref == "special:groups") {
            GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            if (ref == "special:recent") recent_box = b; else groups_box = b;
            gtk_box_append(GTK_BOX(page), b);
            return;
         }
         if (ref == "special:range-pair") {
            // a real toggle, kept in step with the classic toolbar's Range/Pair button
            GtkWidget *orig = widget_from_builder("select_residue_range_toggle_button");
            GtkWidget *t = gtk_toggle_button_new();
            gtk_button_set_child(GTK_BUTTON(t), tool_button_content("range-pair", "Pick Residue Range", ""));
            gtk_widget_add_css_class(t, "flat");
            gtk_widget_add_css_class(t, "coot-plus-tool");
            gtk_widget_set_focusable(t, FALSE);
            auto it = descriptions().find("tb:select_residue_range_toggle_button");
            if (it != descriptions().end()) gtk_widget_set_tooltip_text(t, it->second.c_str());
            if (orig && GTK_IS_TOGGLE_BUTTON(orig)) {
               g_object_bind_property(orig, "active", t, "active",
                                      GBindingFlags(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
            }
            gtk_box_append(GTK_BOX(page), t);
            return;
         }
         if (ref == "special:clear-drawn") {
            command_t c;
            c.label = "Clear Dots and Overlaps";
            c.icon = "broom";
            c.description = "Removes the extra drawn objects - contact dots, overlap spheres and other markers - "
                            "from the view. The model is not changed.";
            c.run = [] () { clear_drawn_objects(); };
            gtk_box_append(GTK_BOX(page), make_tool_button(c));
            return;
         }
         if (ref == "special:classic-toolbar") {
            GtkWidget *b = gtk_button_new_with_label("Show the classic toolbar");
            gtk_widget_add_css_class(b, "flat");
            gtk_widget_add_css_class(b, "coot-plus-link");
            gtk_widget_set_focusable(b, FALSE);
            gtk_widget_set_tooltip_text(b, "Show Coot's original column of build buttons on the right as well");
            g_signal_connect(b, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) {
                                set_classic_toolbar_visible(true); }), nullptr);
            gtk_box_append(GTK_BOX(page), b);
            return;
         }
         std::vector<std::string> parts = split_bar(ref);
         if (parts[0].rfind("menu:", 0) == 0) {
            GtkWidget *mb = make_menu_tool_button(parts[0].substr(5), parts.size() > 1 ? parts[1] : "More",
                                                  parts.size() > 2 ? parts[2] : "");
            if (mb) gtk_box_append(GTK_BOX(page), mb);
            return;
         }
         const command_t *c = resolve_tool(ref);
         if (!c) {
            g_debug("coot-plus: task tool %s not found", ref.c_str());
            return;
         }
         std::string key = c->label + "|" + c->path + "|" + c->action + "|" + c->target;
         if (added.count(key)) return;
         added.insert(key);
         gtk_box_append(GTK_BOX(page), make_tool_button(*c, parts.size() > 1 ? parts[1] : ""));
         return;
            };

      building_task = task.name;
      auto cit = common_tools().find(task.name);
      GtkWidget *full_box = page;
      if (cit != common_tools().end()) {
         std::vector<std::string> custom = custom_common(task.name);
         if (cit->second.empty() || cit->second[0].rfind("section:", 0) != 0 || !custom.empty())
            gtk_box_append(GTK_BOX(page), section_label(custom.empty() ? "Common" : "Common (yours)"));
         const std::vector<std::string> &entries = custom.empty() ? cit->second : custom;
         for (const auto &e : entries) {
            if (e.rfind("cmd:", 0) == 0) {
               size_t sep = e.find('\x1f');
               std::string key = e.substr(4, sep == std::string::npos ? std::string::npos : sep - 4);
               std::string label = sep == std::string::npos ? std::string() : e.substr(sep + 1);
               for (const auto &cmd : commands)
                  if (command_key(cmd) == key) { gtk_box_append(GTK_BOX(page), make_tool_button(cmd, label, true)); break; }
            } else {
               append_entry(page, e, true);
            }
         }
         added.clear();
         GtkWidget *exp = gtk_expander_new(("All " + task.name + " tools").c_str());
         gtk_widget_add_css_class(exp, "coot-plus-expander");
         std::string key = "expand-" + task.name;
         gtk_expander_set_expanded(GTK_EXPANDER(exp), load_coot_plus_setting_bool(key.c_str(), false));
         g_object_set_data_full(G_OBJECT(exp), "setting", g_strdup(key.c_str()), g_free);
         g_signal_connect(exp, "notify::expanded", G_CALLBACK(+[] (GObject *o, GParamSpec *, gpointer) {
                             save_coot_plus_setting_bool(static_cast<const char *>(g_object_get_data(o, "setting")),
                                                         gtk_expander_get_expanded(GTK_EXPANDER(o))); }), nullptr);
         full_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
         gtk_expander_set_child(GTK_EXPANDER(exp), full_box);
         gtk_box_append(GTK_BOX(page), exp);
      }
      for (const auto &ref : task.tools) {
         // the custom widgets live in the Common list only (one of each)
         if (ref.rfind("special:", 0) == 0 && ref != "special:classic-toolbar" && ref != "special:maps-hint" &&
             cit != common_tools().end()) {
            bool in_common = std::find(cit->second.begin(), cit->second.end(), ref) != cit->second.end();
            if (in_common) continue;
         }
         if (cit != common_tools().end() && ref.rfind("special:", 0) == 0 &&
             std::find(cit->second.begin(), cit->second.end(), ref) != cit->second.end()) continue;
         append_entry(full_box, ref, true);
      }

      GtkWidget *scrolled = gtk_scrolled_window_new();
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
      gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), page);
      gtk_stack_add_named(GTK_STACK(stack), scrolled, task.name.c_str());
   }

   g_signal_connect(task_list, "row-selected",
                    G_CALLBACK(+[] (GtkListBox *, GtkListBoxRow *row, gpointer stack) {
                       if (row) {
                          gtk_stack_set_visible_child_name(GTK_STACK(stack), gtk_widget_get_name(GTK_WIDGET(row)));
                          if (std::string(gtk_widget_get_name(GTK_WIDGET(row))) == "Load") refresh_recent_and_groups();
                       }
                    }), stack);
   load_favourites();
   rebuild_favourites();
   gtk_list_box_select_row(GTK_LIST_BOX(task_list),
                           gtk_list_box_get_row_at_index(GTK_LIST_BOX(task_list), favourites.empty() ? 1 : 0));

   gtk_box_append(GTK_BOX(outer), task_list);
   GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
   gtk_widget_add_css_class(sep, "coot-plus-sidebar-sep");
   gtk_box_append(GTK_BOX(outer), sep);
   gtk_widget_set_vexpand(stack, TRUE);
   gtk_box_append(GTK_BOX(outer), stack);

   GtkWidget *help = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
   gtk_widget_add_css_class(help, "coot-plus-help");
   help_title = gtk_label_new("");
   help_text  = gtk_label_new("");
   gtk_label_set_xalign(GTK_LABEL(help_title), 0.0);
   gtk_label_set_xalign(GTK_LABEL(help_text), 0.0);
   gtk_label_set_wrap(GTK_LABEL(help_text), TRUE);
   gtk_label_set_max_width_chars(GTK_LABEL(help_text), 28);
   gtk_label_set_lines(GTK_LABEL(help_text), 6);
   gtk_label_set_ellipsize(GTK_LABEL(help_text), PANGO_ELLIPSIZE_END);
   gtk_widget_add_css_class(help_title, "coot-plus-help-title");
   gtk_widget_add_css_class(help_text,  "coot-plus-help-text");
   gtk_widget_set_size_request(help, -1, 96);
   gtk_box_append(GTK_BOX(help), help_title);
   gtk_box_append(GTK_BOX(help), help_text);
   gtk_box_append(GTK_BOX(outer), help);
   sidebar_help_box = help;
   help_default_title = "Tip";
   help_default_text  = "Point at a tool to see what it does. Most tools act on the residue at the centre of the "
                        "screen: Option-click an atom to centre on it first (or use Go To Atom).";
   show_default_help();
}

// ------------------------------------------------------------------------------------
//                                  context inspector
// ------------------------------------------------------------------------------------

GtkWidget *inspector             = nullptr;
GtkWidget *sel_title_label       = nullptr;
GtkWidget *sel_detail_label      = nullptr;
GtkWidget *symm_radius_scale     = nullptr;
GtkWidget *symm_radius_label     = nullptr;
GtkWidget *unit_cell_check       = nullptr;
GtkWidget *label_size_label      = nullptr;
std::string maps_signature;
std::string selection_signature;

std::string format_float(double v, int decimals);

// A Blender-style number: drag on it (left/right or up/down) to change the value,
// Shift for fine steps, click to type a value, Esc cancels typing.
struct drag_number_t {
   GtkWidget *stack = nullptr, *label = nullptr, *entry = nullptr;
   double value = 0, lo = 0, hi = 1, per_pixel = 0.01, start = 0;
   int decimals = 2;
   std::string prefix, suffix;
   bool moved = false;
   std::function<void(double)> on_change;
};

void drag_number_show(drag_number_t *dn) {
   gtk_label_set_text(GTK_LABEL(dn->label), (dn->prefix + format_float(dn->value, dn->decimals) + dn->suffix).c_str());
}

// set from outside (follow changes made elsewhere); does not call on_change
void drag_number_set(drag_number_t *dn, double v) {
   if (!dn) return;
   if (gtk_stack_get_visible_child(GTK_STACK(dn->stack)) == dn->entry) return; // being typed in
   if (std::abs(v - dn->value) < 1e-6) return;
   dn->value = v;
   drag_number_show(dn);
}

void drag_number_change(drag_number_t *dn, double v) {
   v = std::max(dn->lo, std::min(dn->hi, v));
   dn->value = v;
   drag_number_show(dn);
   if (dn->on_change) dn->on_change(v);
}

void drag_number_stop_typing(drag_number_t *dn, bool apply) {
   if (gtk_stack_get_visible_child(GTK_STACK(dn->stack)) != dn->entry) return;
   if (apply) {
      std::string t = gtk_editable_get_text(GTK_EDITABLE(dn->entry));
      char *end = nullptr;
      double v = strtod(t.c_str(), &end);
      if (end && end != t.c_str()) drag_number_change(dn, v);
   }
   gtk_stack_set_visible_child(GTK_STACK(dn->stack), dn->label);
}

GtkWidget *make_drag_number(drag_number_t *dn, const std::string &tooltip) {
   dn->stack = gtk_stack_new();
   gtk_widget_set_halign(dn->stack, GTK_ALIGN_START);
   dn->label = gtk_label_new("");
   gtk_widget_add_css_class(dn->label, "coot-plus-drag-number");
   gtk_widget_set_cursor_from_name(dn->label, "ew-resize");
   gtk_widget_set_tooltip_text(dn->label, (tooltip + "\nDrag left/right or up/down to change it (Shift: fine). "
                                                   "Click to type a value.").c_str());
   dn->entry = gtk_entry_new();
   gtk_editable_set_width_chars(GTK_EDITABLE(dn->entry), 6);
   gtk_widget_add_css_class(dn->entry, "coot-plus-drag-entry");
   gtk_stack_add_child(GTK_STACK(dn->stack), dn->label);
   gtk_stack_add_child(GTK_STACK(dn->stack), dn->entry);
   gtk_stack_set_visible_child(GTK_STACK(dn->stack), dn->label);
   drag_number_show(dn);
   g_object_set_data_full(G_OBJECT(dn->stack), "coot-plus-drag-number", dn,
                          +[] (gpointer d) { delete static_cast<drag_number_t *>(d); });

   GtkGesture *drag = gtk_gesture_drag_new();
   g_signal_connect(drag, "drag-begin", G_CALLBACK(+[] (GtkGestureDrag *, double, double, gpointer d) {
                       auto *dn = static_cast<drag_number_t *>(d); dn->start = dn->value; dn->moved = false; }), dn);
   g_signal_connect(drag, "drag-update", G_CALLBACK(+[] (GtkGestureDrag *g, double dx, double dy, gpointer d) {
                       auto *dn = static_cast<drag_number_t *>(d);
                       double px = std::abs(dx) > std::abs(dy) ? dx : -dy;   // right or up = more
                       if (!dn->moved && std::abs(px) < 3) return;
                       dn->moved = true;
                       GdkModifierType st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
                       double rate = dn->per_pixel * ((st & GDK_SHIFT_MASK) ? 0.1 : 1.0);
                       drag_number_change(dn, dn->start + px * rate); }), dn);
   g_signal_connect(drag, "drag-end", G_CALLBACK(+[] (GtkGestureDrag *, double, double, gpointer d) {
                       auto *dn = static_cast<drag_number_t *>(d);
                       if (dn->moved) return;
                       gtk_editable_set_text(GTK_EDITABLE(dn->entry), format_float(dn->value, dn->decimals).c_str());
                       gtk_stack_set_visible_child(GTK_STACK(dn->stack), dn->entry);
                       gtk_widget_grab_focus(dn->entry);
                       gtk_editable_select_region(GTK_EDITABLE(dn->entry), 0, -1); }), dn);
   gtk_widget_add_controller(dn->label, GTK_EVENT_CONTROLLER(drag));

   g_signal_connect(dn->entry, "activate", G_CALLBACK(+[] (GtkEntry *, gpointer d) {
                       drag_number_stop_typing(static_cast<drag_number_t *>(d), true); }), dn);
   GtkEventController *focus = gtk_event_controller_focus_new();
   g_signal_connect(focus, "leave", G_CALLBACK(+[] (GtkEventControllerFocus *, gpointer d) {
                       drag_number_stop_typing(static_cast<drag_number_t *>(d), true); }), dn);
   gtk_widget_add_controller(dn->entry, focus);
   GtkEventController *keys = gtk_event_controller_key_new();
   g_signal_connect(keys, "key-pressed", G_CALLBACK(+[] (GtkEventControllerKey *, guint kv, guint, GdkModifierType, gpointer d) -> gboolean {
                       if (kv != GDK_KEY_Escape) return FALSE;
                       drag_number_stop_typing(static_cast<drag_number_t *>(d), false); return TRUE; }), dn);
   gtk_widget_add_controller(dn->entry, keys);
   return dn->stack;
}

// contour and opacity numbers on each map row of the Molecules list
struct map_row_t {
   int imol;
   drag_number_t *contour;
   drag_number_t *opacity;
   GtkWidget *absolute;
};
std::vector<map_row_t> map_rows;
drag_number_t *map_radius_number = nullptr;

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
   float a = get_contour_level_absolute(r.imol);
   gtk_label_set_text(GTK_LABEL(r.absolute), (format_float(a, 3) + " e/Å³").c_str());
}

// follow changes made elsewhere (scroll wheel, +/- keys, Map Parameters dialog)
void update_maps_section() {
   for (const auto &r : map_rows) {
      if (!is_valid_map_molecule(r.imol)) continue;
      drag_number_set(r.contour, get_contour_level_in_sigma(r.imol));
      drag_number_set(r.opacity, 100.0 * get_solid_density_surface_opacity(r.imol));
      update_map_readout(r);
   }
}

void update_misc_section() {
   updating_widgets = true;
   drag_number_set(map_radius_number, get_map_radius());

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
   ignore_history_scope ignore;
   if (!gtk_widget_get_mapped(inspector)) return G_SOURCE_CONTINUE;
   update_molecules_section();
   update_selection_section();
   update_sequence_strip();
   update_ligand_card();
   update_maps_section();
   update_misc_section();
   sync_scroll_buttons();
   return G_SOURCE_CONTINUE;
}

// ------------------------------------------------------------------------------------
//                 Undo View: undo display changes (not model edits)
// ------------------------------------------------------------------------------------
//
// Coot's Undo only reverses changes to the model. Display changes - dots and overlap
// spheres, ribbons and surfaces, show/hide, contour levels, symmetry, drawing style,
// atom labels - had no undo. The display state is sampled twice a second; when it
// changes, the previous state goes on a stack (changes of the same kind within 1.5 s,
// e.g. scrolling the contour, are merged). Undo View restores the last state.
// Opening or closing files is not a display change and is not undone.

struct view_mol_t {
   int imol = -1;
   bool model = false;
   std::string name;
   bool shown = false, active = false, symm = false;
   float sigma = 0;
   int bond_type = 0;
   std::vector<bool> mesh_draw;     // molecule meshes (ribbons, surfaces...)
   std::vector<bool> inst_draw;     // instanced meshes (contact dots, clashes...)
   std::vector<int> labels;
};

struct view_state_t {
   std::vector<view_mol_t> mols;
   std::vector<int> generic; // per generic display object: -1 closed, 0 hidden, 1 shown
   bool symm_master = false, overlaps = false;
   float symm_radius = 0, map_radius = 0;
   int font_size = 2;
   std::string what; // the kind of change that followed this state (for the message)
};

std::vector<view_state_t> view_undo_stack;
view_state_t view_last;
bool view_last_valid = false;
gint64 view_last_push_time = 0;

view_state_t capture_view_state() {
   view_state_t v;
   for (int imol=0; imol<graphics_n_molecules(); imol++) {
      bool model = is_valid_model_molecule(imol), map = is_valid_map_molecule(imol);
      if (!model && !map) continue;
      molecule_class_info_t &m = graphics_info_t::molecules[imol];
      view_mol_t vm;
      vm.imol = imol; vm.model = model; vm.name = molecule_name(imol);
      vm.shown = model ? mol_is_displayed(imol) : map_is_displayed(imol);
      if (model) {
         vm.active = mol_is_active(imol);
         vm.symm = m.show_symmetry;
         vm.bond_type = m.Bonds_box_type();
         vm.labels = m.labelled_atom_index_list;
      } else {
         vm.sigma = get_contour_level_in_sigma(imol);
      }
      for (const auto &mesh : m.meshes)          vm.mesh_draw.push_back(!mesh.is_closed() && mesh.get_draw_this_mesh());
      for (const auto &imm  : m.instanced_meshes) vm.inst_draw.push_back(!imm.is_closed() && imm.get_draw_status());
      v.mols.push_back(vm);
   }
   int n_go = number_of_generic_objects();
   for (int i=0; i<n_go; i++)
      v.generic.push_back(is_closed_generic_object_p(i) ? -1 : (generic_object_is_displayed_p(i) ? 1 : 0));
   v.symm_master = get_show_symmetry();
   v.overlaps    = graphics_info_t::show_atom_overlaps_flag;
   v.symm_radius = graphics_info_t::symmetry_search_radius;
   v.map_radius  = get_map_radius();
   v.font_size   = get_font_size();
   return v;
}

bool same_molecule_set(const view_state_t &a, const view_state_t &b) {
   if (a.mols.size() != b.mols.size()) return false;
   for (unsigned int i=0; i<a.mols.size(); i++)
      if (a.mols[i].imol != b.mols[i].imol || a.mols[i].name != b.mols[i].name) return false;
   return true;
}

// what changed between a and b ("" if nothing)
std::string view_difference(const view_state_t &a, const view_state_t &b) {
   if (a.generic != b.generic || a.overlaps != b.overlaps) return "drawn objects (dots, overlaps, markers)";
   for (unsigned int i=0; i<a.mols.size(); i++) {
      const view_mol_t &x = a.mols[i], &y = b.mols[i];
      if (x.mesh_draw != y.mesh_draw || x.inst_draw != y.inst_draw) return "drawn objects (dots, ribbons, surfaces)";
      if (x.shown != y.shown)   return "show/hide " + file_basename(x.name);
      if (x.active != y.active) return "clickable " + file_basename(x.name);
      if (x.symm != y.symm)     return "symmetry for " + file_basename(x.name);
      if (x.bond_type != y.bond_type) return "drawing style of " + file_basename(x.name);
      if (x.labels != y.labels) return "atom labels";
      if (std::abs(x.sigma - y.sigma) > 1e-4) return "contour level of " + file_basename(x.name);
   }
   if (a.symm_master != b.symm_master) return "symmetry on/off";
   if (std::abs(a.symm_radius - b.symm_radius) > 1e-3) return "symmetry radius";
   if (std::abs(a.map_radius - b.map_radius) > 1e-3) return "map radius";
   if (a.font_size != b.font_size) return "label size";
   return "";
}

bool view_restoring = false;

void track_view_changes() {
   if (view_restoring) return;
   ignore_history_scope ignore;
   view_state_t cur = capture_view_state();
   if (!view_last_valid || !same_molecule_set(cur, view_last)) {
      // files opened or closed: new baseline (and older states may not apply any more)
      if (view_last_valid && cur.mols.size() < view_last.mols.size()) view_undo_stack.clear();
      view_last = cur; view_last_valid = true;
      return;
   }
   std::string what = view_difference(view_last, cur);
   if (what.empty()) return;
   gint64 now = g_get_monotonic_time();
   bool merge = !view_undo_stack.empty() && view_undo_stack.back().what == what &&
                (now - view_last_push_time) < 1500000;
   if (!merge) {
      view_last.what = what;
      view_undo_stack.push_back(view_last);
      if (view_undo_stack.size() > 50) view_undo_stack.erase(view_undo_stack.begin());
   }
   view_last_push_time = now;
   view_last = cur;
}

void apply_view_state(const view_state_t &s) {
   view_restoring = true;
   // generic display objects: close the ones made since, restore shown/hidden
   int n_go = number_of_generic_objects();
   for (int i=0; i<n_go; i++) {
      if (is_closed_generic_object_p(i)) continue;
      if (i >= static_cast<int>(s.generic.size()) || s.generic[i] == -1)
         close_generic_object(i);
      else
         set_display_generic_object_simple(i, s.generic[i]);
   }
   graphics_info_t g;
   g.set_show_atom_overlaps(s.overlaps);
   if (graphics_info_t::atom_overlaps_toggle_button)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(graphics_info_t::atom_overlaps_toggle_button), s.overlaps);

   for (const view_mol_t &vm : s.mols) {
      int imol = vm.imol;
      bool model = is_valid_model_molecule(imol), map = is_valid_map_molecule(imol);
      if (!(model || map) || molecule_name(imol) != vm.name) continue;
      molecule_class_info_t &m = graphics_info_t::molecules[imol];
      if (model) {
         if (mol_is_displayed(imol) != vm.shown) set_mol_displayed(imol, vm.shown);
         if (mol_is_active(imol) != vm.active)   set_mol_active(imol, vm.active);
         if (m.show_symmetry != vm.symm)         set_show_symmetry_molecule(imol, vm.symm);
         if (m.labelled_atom_index_list != vm.labels) m.labelled_atom_index_list = vm.labels;
         if (m.Bonds_box_type() != vm.bond_type) {
            // the two common styles can be set back through the public API
            if (vm.bond_type == coot::NORMAL_BONDS) graphics_to_bonds_representation(imol);
            if (vm.bond_type == coot::CA_BONDS)     graphics_to_ca_representation(imol);
         }
      } else {
         if (map_is_displayed(imol) != vm.shown) set_map_displayed(imol, vm.shown);
         if (std::abs(get_contour_level_in_sigma(imol) - vm.sigma) > 1e-4) set_contour_level_in_sigma(imol, vm.sigma);
      }
      for (unsigned int j=0; j<m.meshes.size(); j++) {
         if (m.meshes[j].is_closed()) continue;
         if (j >= vm.mesh_draw.size()) m.meshes[j].close();
         else m.meshes[j].set_draw_mesh_state(vm.mesh_draw[j]);
      }
      for (unsigned int j=0; j<m.instanced_meshes.size(); j++) {
         if (m.instanced_meshes[j].is_closed()) continue;
         if (j >= vm.inst_draw.size()) m.instanced_meshes[j].close();
         else m.instanced_meshes[j].set_draw_status(vm.inst_draw[j]);
      }
   }
   if (get_show_symmetry() != s.symm_master) set_show_symmetry_master(s.symm_master);
   if (std::abs(graphics_info_t::symmetry_search_radius - s.symm_radius) > 1e-3) set_symmetry_size(s.symm_radius);
   if (std::abs(get_map_radius() - s.map_radius) > 1e-3) set_map_radius(s.map_radius);
   if (get_font_size() != s.font_size) set_font_size(s.font_size);
   graphics_draw();
   view_last = capture_view_state();
   view_last_valid = true;
   view_restoring = false;
}

void undo_view_change() {
   track_view_changes(); // catch a change made in the last half second
   if (view_undo_stack.empty()) {
      graphics_info_t::add_status_bar_text("No display changes to undo (Undo View undoes dots, overlaps, surfaces, "
                                           "show/hide, contours, symmetry, labels - model edits use Undo)");
      return;
   }
   view_state_t s = view_undo_stack.back();
   view_undo_stack.pop_back();
   apply_view_state(s);
   graphics_info_t::add_status_bar_text("Undid display change: " + s.what);
}

void clear_drawn_objects() {
   close_all_generic_objects(); // generic objects, contact dots, overlap spheres (and syncs the overlaps toggle)
   graphics_info_t::add_status_bar_text("Cleared the extra drawn objects (dots, overlaps, markers) - Undo View brings them back only if hidden, not cleared");
}

// ------------------------------------------------------------------------------------
//                     Molecules: every open model and map, with show/close
// ------------------------------------------------------------------------------------

GtkWidget *molecules_box = nullptr;
std::string molecules_signature;
struct molecule_row_t { int imol; GtkWidget *shown; };
std::vector<molecule_row_t> molecule_rows;

std::vector<float> model_cell(int imol) {
   std::pair<std::vector<float>, std::string> cs = graphics_info_t::molecules[imol].get_cell_and_symm();
   return cs.first;
}

std::string model_spacegroup(int imol) {
   return graphics_info_t::molecules[imol].get_cell_and_symm().second;
}

std::vector<float> map_cell(int imol) {
   std::vector<float> v;
   const molecule_class_info_t &m = graphics_info_t::molecules[imol];
   if (!m.has_xmap()) return v;
   const clipper::Cell &c = m.xmap.cell();
   v = {float(c.a()), float(c.b()), float(c.c()),
        float(clipper::Util::rad2d(c.alpha())), float(clipper::Util::rad2d(c.beta())),
        float(clipper::Util::rad2d(c.gamma()))};
   return v;
}

bool cells_match(const std::vector<float> &a, const std::vector<float> &b) {
   if (a.size() < 6 || b.size() < 6) return true; // can't tell
   for (int i=0; i<3; i++)
      if (std::abs(a[i] - b[i]) > 0.02 * std::max(a[i], b[i])) return false; // 2% on lengths
   for (int i=3; i<6; i++)
      if (std::abs(a[i] - b[i]) > 1.5) return false; // 1.5 degrees
   return true;
}

void close_molecule_confirmed(int imol); // below
void close_everything_confirmed();

void ask_then(const std::string &question, const std::string &detail, const char *ok_label,
              void (*on_ok)(int), int imol) {
   GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", question.c_str());
   gtk_alert_dialog_set_detail(dialog, detail.c_str());
   const char *buttons[] = {"Cancel", ok_label, nullptr};
   gtk_alert_dialog_set_buttons(dialog, buttons);
   gtk_alert_dialog_set_cancel_button(dialog, 0);
   gtk_alert_dialog_set_default_button(dialog, 1);
   struct ctx_t { void (*f)(int); int imol; };
   ctx_t *ctx = new ctx_t{on_ok, imol};
   GtkWidget *w = graphics_info_t::get_main_window();
   gtk_alert_dialog_choose(dialog, w ? GTK_WINDOW(w) : nullptr, nullptr,
                           +[] (GObject *src, GAsyncResult *res, gpointer data) {
                              ctx_t *c = static_cast<ctx_t *>(data);
                              int r = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
                              if (r == 1) c->f(c->imol);
                              delete c;
                           }, ctx);
   g_object_unref(dialog);
}

void close_molecule_confirmed(int imol) {
   std::string name = file_basename(molecule_name(imol));
   close_molecule(imol);
   graphics_info_t::add_status_bar_text("Closed " + name);
   graphics_draw();
}

void close_everything_confirmed() {
   for (int imol=0; imol<graphics_n_molecules(); imol++)
      if (is_valid_model_molecule(imol) || is_valid_map_molecule(imol))
         close_molecule(imol);
   graphics_info_t::add_status_bar_text("Closed all models and maps");
   graphics_draw();
}

void ask_close_molecule(int imol) {
   std::string name = file_basename(molecule_name(imol));
   bool model = is_valid_model_molecule(imol);
   std::string detail = "This removes it from Coot. The file on disk is not changed.";
   if (model && graphics_info_t::molecules[imol].Have_unsaved_changes_p())
      detail = "This model has changes that have not been saved - they will be lost.\n"
               "(Save it first with Save > Save Coordinates.)";
   ask_then(std::string("Close ") + (model ? "model " : "map ") + name + "?", detail, "Close",
            [] (int i) { close_molecule_confirmed(i); }, imol);
}

void ask_close_everything() {
   bool unsaved = false;
   for (int imol=0; imol<graphics_n_molecules(); imol++)
      if (is_valid_model_molecule(imol) && graphics_info_t::molecules[imol].Have_unsaved_changes_p()) unsaved = true;
   ask_then("Close all models and maps?",
            unsaved ? "Some models have unsaved changes - they will be lost."
                    : "Files on disk are not changed.", "Close All",
            [] (int) { close_everything_confirmed(); }, -1);
}

// a flat button in a row menu; runs f(imol) and closes the popover
void add_menu_entry(GtkWidget *box, GtkWidget *popover, const char *label, const char *tooltip,
                    void (*f)(int), int imol) {
   GtkWidget *b = gtk_button_new_with_label(label);
   gtk_widget_add_css_class(b, "flat");
   gtk_widget_add_css_class(b, "coot-plus-menu-entry");
   gtk_widget_set_focusable(b, FALSE);
   if (tooltip) gtk_widget_set_tooltip_text(b, tooltip);
   GtkWidget *l = gtk_button_get_child(GTK_BUTTON(b));
   if (GTK_IS_LABEL(l)) gtk_label_set_xalign(GTK_LABEL(l), 0.0);
   g_object_set_data(G_OBJECT(b), "imol", GINT_TO_POINTER(imol));
   g_object_set_data(G_OBJECT(b), "popover", popover);
   g_object_set_data(G_OBJECT(b), "func", reinterpret_cast<gpointer>(f));
   g_signal_connect(b, "clicked", G_CALLBACK(+[] (GtkButton *btn, gpointer) {
                       auto fn = reinterpret_cast<void (*)(int)>(g_object_get_data(G_OBJECT(btn), "func"));
                       int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "imol"));
                       gtk_popover_popdown(GTK_POPOVER(g_object_get_data(G_OBJECT(btn), "popover")));
                       fn(i);
                       give_focus_back_to_graphics();
                    }), nullptr);
   gtk_box_append(GTK_BOX(box), b);
}

void open_display_manager() {
   GtkWidget *w = widget_from_builder("toolbar_display_manager_button");
   if (w) g_signal_emit_by_name(w, "clicked");
}

GtkWidget *make_molecule_menu(int imol, bool model) {
   GtkWidget *mb = gtk_menu_button_new();
   gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(mb), "coot-plus-more-symbolic");
   gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(mb), FALSE);
   gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(mb), FALSE);
   gtk_widget_set_focusable(mb, FALSE);
   gtk_widget_set_tooltip_text(mb, "More for this one");
   GtkWidget *pop = gtk_popover_new();
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
   add_menu_entry(box, pop, "Label…", "Give it your own name in this list (remembered for this file)",
                  [] (int i) { ask_rename_molecule(i); }, imol);
   if (model) {
      add_menu_entry(box, pop, "Centre on this model", nullptr, [] (int i) {
            set_rotation_centre(molecule_centre_internal(i, 0), molecule_centre_internal(i, 1),
                                molecule_centre_internal(i, 2)); }, imol);
      add_menu_entry(box, pop, "Show only this model", "Hide all the other models", [] (int i) {
            set_display_only_model_mol(i); }, imol);
      add_menu_entry(box, pop, mol_is_active(imol) ? "Make not clickable" : "Make clickable",
                     "Clickable (active) models are the ones picking and model-building tools work on", [] (int i) {
            set_mol_active(i, !mol_is_active(i)); molecules_signature.clear(); }, imol);
      add_menu_entry(box, pop, graphics_info_t::molecules[imol].show_symmetry ? "Symmetry off for this model"
                                                                           : "Symmetry on for this model",
                     "Show or hide the crystal neighbours of this model only", [] (int i) {
            bool on = !graphics_info_t::molecules[i].show_symmetry;
            set_show_symmetry_molecule(i, on);
            if (on) set_show_symmetry_master(1);
            molecules_signature.clear(); graphics_draw(); }, imol);
   } else {
      add_menu_entry(box, pop, "Use this map for refinement", "Real-space refinement and fitting will use this map",
                     [] (int i) { set_imol_refinement_map(i);
                        graphics_info_t::add_status_bar_text("Refinement map: " + file_basename(molecule_name(i))); }, imol);
      add_menu_entry(box, pop, "Scroll wheel contours this map", nullptr, [] (int i) {
            set_scrollable_map(i);
            graphics_info_t::add_status_bar_text("Scroll wheel map: " + file_basename(molecule_name(i))); }, imol);
   }
   if (model) add_model_look_entries(box, pop, imol);
   else       add_map_look_entries(box, pop, imol);
   gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
   add_menu_entry(box, pop, "Open the full Display Manager", "Every display option for every molecule (F8)",
                  [] (int) { open_display_manager(); }, imol);
   add_menu_entry(box, pop, "Close…", nullptr, [] (int i) { ask_close_molecule(i); }, imol);
   GtkWidget *msw = gtk_scrolled_window_new();
   gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(msw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(msw), 560);
   gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(msw), TRUE);
   gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(msw), TRUE);
   gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(msw), box);
   gtk_popover_set_child(GTK_POPOVER(pop), msw);
   gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), pop);
   return mb;
}

// ---- names that say what each molecule is

// what an MTZ map is, from its amplitude column (the usual Refmac/Phenix/Buster names)
std::string map_kind(int imol) {
   const molecule_class_info_t &m = graphics_info_t::molecules[imol];
   bool diff = map_is_difference_map(imol);
   std::string f = m.save_f_col;
   std::string fu;
   for (char c : f) fu += std::toupper(static_cast<unsigned char>(c));
   if (fu.empty()) return diff ? "Difference map" : "Map";
   static const std::vector<std::pair<std::string, std::string> > kinds = {
      {"2FOFCWT", "2mFo-DFc"}, {"FWT", "2mFo-DFc"}, {"2FOFC", "2mFo-DFc"},
      {"FOFCWT", "mFo-DFc"}, {"DELFWT", "mFo-DFc"}, {"FOFC", "mFo-DFc"},
      {"FAN", "Anomalous"}, {"ANOM", "Anomalous"}, {"FC", "Fcalc"}};
   for (const auto &k : kinds)
      if (fu == k.first || fu.rfind(k.first + "_", 0) == 0 || fu.rfind(k.first + "-", 0) == 0) return k.second;
   return f; // unknown column name: show it as it is
}

// a key for remembering a label across sessions (and groups): the file (+ column for MTZ maps)
std::string source_key(int imol) {
   std::error_code ec;
   const molecule_class_info_t &m = graphics_info_t::molecules[imol];
   if (is_valid_map_molecule(imol) && !m.save_mtz_file_name.empty())
      return std::filesystem::absolute(m.save_mtz_file_name, ec).lexically_normal().string() + "|" + m.save_f_col;
   return std::filesystem::absolute(molecule_name(imol), ec).lexically_normal().string();
}

nlohmann::json labels_cache;
gint64 labels_cache_time = 0;

std::string custom_label(int imol) {
   gint64 now = g_get_monotonic_time();
   if (labels_cache_time == 0 || now - labels_cache_time > 2 * G_USEC_PER_SEC) {
      labels_cache = read_json_file(config_path("labels.json"), nlohmann::json::object());
      labels_cache_time = now;
   }
   std::string k = source_key(imol);
   return (labels_cache.contains(k) && labels_cache[k].is_string()) ? labels_cache[k].get<std::string>() : std::string();
}

void set_custom_label(int imol, const std::string &label) {
   nlohmann::json j = read_json_file(config_path("labels.json"), nlohmann::json::object());
   std::string k = source_key(imol);
   if (label.empty()) j.erase(k); else j[k] = label;
   write_json_file(config_path("labels.json"), j);
   labels_cache_time = 0; // re-read next time
   molecules_signature.clear();
   maps_signature.clear();
}

// what the Molecules list and the Maps card call it
std::string display_name(int imol) {
   std::string l = custom_label(imol);
   if (!l.empty()) return l;
   if (is_valid_map_molecule(imol)) {
      std::string k = map_kind(imol);
      return map_is_difference_map(imol) && k == "mFo-DFc" ? "mFo-DFc (difference)" : k;
   }
   return file_basename(molecule_name(imol));
}

void ask_rename_molecule(int imol) {
   std::string current = display_name(imol);
   const molecule_class_info_t &m = graphics_info_t::molecules[imol];
   std::string what = file_basename(m.save_mtz_file_name.empty() ? molecule_name(imol) : m.save_mtz_file_name);
   if (is_valid_map_molecule(imol) && !m.save_f_col.empty()) what += " (" + m.save_f_col + ")";
   ask_name(std::string("Label ") + (is_valid_model_molecule(imol) ? "Model" : "Map"),
            "Label for " + what + "\n(clear it to go back to the automatic name)",
            current, [imol] (const std::string &n) {
               set_custom_label(imol, n);
               graphics_info_t::add_status_bar_text(n.empty() ? "Label removed" : "Label: " + n);
            }, true);
}

GtkWidget *molecule_section_heading(const std::string &text) {
   GtkWidget *l = section_label(text);
   gtk_widget_set_margin_top(l, 6);
   return l;
}
GtkWidget *make_molecule_row(int imol, int ref_model, const std::vector<float> &ref_cell) {
   bool model = is_valid_model_molecule(imol);
   bool diff = !model && map_is_difference_map(imol);

   GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_widget_add_css_class(row, "coot-plus-molecule-row");

   GtkWidget *shown = gtk_check_button_new();
   gtk_widget_set_focusable(shown, FALSE);
   gtk_widget_set_valign(shown, GTK_ALIGN_START);
   gtk_check_button_set_active(GTK_CHECK_BUTTON(shown), model ? mol_is_displayed(imol) : map_is_displayed(imol));
   gtk_widget_set_tooltip_text(shown, "Show / hide");
   g_signal_connect(shown, "toggled", G_CALLBACK(+[] (GtkCheckButton *b, gpointer d) {
                       if (updating_widgets) return;
                       int i = GPOINTER_TO_INT(d);
                       if (is_valid_model_molecule(i)) set_mol_displayed(i, gtk_check_button_get_active(b));
                       else set_map_displayed(i, gtk_check_button_get_active(b));
                       molecules_signature.clear(); // the cell check depends on what is shown
                    }), GINT_TO_POINTER(imol));
   molecule_rows.push_back(molecule_row_t{imol, shown});

   // what it is, at a glance: type tag above the colour swatch
   GtkWidget *type_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
   gtk_widget_set_valign(type_box, GTK_ALIGN_START);
   GtkWidget *tag = gtk_label_new(model ? "MODEL" : (diff ? "Δ MAP" : "MAP"));
   gtk_widget_add_css_class(tag, "coot-plus-type-tag");
   gtk_widget_add_css_class(tag, model ? "coot-plus-type-model" : (diff ? "coot-plus-type-diff" : "coot-plus-type-map"));
   gtk_box_append(GTK_BOX(type_box), tag);
   gtk_widget_set_size_request(type_box, 40, -1);

   GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
   gtk_widget_set_hexpand(text, TRUE);
   std::string title = display_name(imol);
   GtkWidget *name_label = gtk_label_new(title.c_str());
   gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
   gtk_label_set_ellipsize(GTK_LABEL(name_label), PANGO_ELLIPSIZE_END);
   gtk_label_set_max_width_chars(GTK_LABEL(name_label), 24);
   gtk_widget_add_css_class(name_label, "coot-plus-map-name");
   gtk_box_append(GTK_BOX(text), name_label);

   std::string info, warning;
   const molecule_class_info_t &m = graphics_info_t::molecules[imol];
   if (model) {
      std::string base = file_basename(molecule_name(imol));
      if (title != base) info = base + " · ";
      info += "#" + std::to_string(imol);
      std::string sg = model_spacegroup(imol);
      if (!sg.empty()) info += " · " + sg;
      info += " · " + std::to_string(m.atom_sel.n_selected_atoms) + " atoms";
      if (!mol_is_active(imol)) info += " · not clickable";
      if (ref_model >= 0 && imol != ref_model && !cells_match(model_cell(imol), ref_cell))
         warning = "Different unit cell from " + display_name(ref_model);
   } else {
      std::string file = file_basename(m.save_mtz_file_name.empty() ? molecule_name(imol) : m.save_mtz_file_name);
      info = file + " · #" + std::to_string(imol);
      if (imol == imol_refinement_map()) info += " · refinement map";
      if (ref_model >= 0 && !cells_match(map_cell(imol), ref_cell))
         warning = "Unit cell differs from " + display_name(ref_model) + " - probably a different crystal";
   }
   GtkWidget *info_label = gtk_label_new(info.c_str());
   gtk_label_set_xalign(GTK_LABEL(info_label), 0.0);
   gtk_label_set_wrap(GTK_LABEL(info_label), TRUE);
   gtk_label_set_wrap_mode(GTK_LABEL(info_label), PANGO_WRAP_WORD_CHAR);
   gtk_label_set_lines(GTK_LABEL(info_label), 2);
   gtk_label_set_ellipsize(GTK_LABEL(info_label), PANGO_ELLIPSIZE_END);
   gtk_label_set_max_width_chars(GTK_LABEL(info_label), 24);
   gtk_widget_add_css_class(info_label, "coot-plus-dim");
   gtk_box_append(GTK_BOX(text), info_label);
   std::string tip = molecule_name(imol);
   if (!model && !m.save_f_col.empty()) tip = m.save_mtz_file_name + "\ncolumns " + m.save_f_col + " " + m.save_phi_col;
   tip += "\nDouble-click the name to give it your own label";
   gtk_widget_set_tooltip_text(text, tip.c_str());
   if (!warning.empty()) {
      GtkWidget *w = gtk_label_new(("⚠ " + warning).c_str());
      gtk_label_set_xalign(GTK_LABEL(w), 0.0);
      gtk_label_set_wrap(GTK_LABEL(w), TRUE);
      gtk_label_set_max_width_chars(GTK_LABEL(w), 24);
      gtk_widget_add_css_class(w, "coot-plus-warning");
      gtk_widget_set_tooltip_text(w, "The unit cells don't match, so this was most likely made from a different "
                                     "data set or crystal than the model. Close it with ✕ if it's not wanted.");
      gtk_box_append(GTK_BOX(text), w);
   }
   // double-click the name: label it
   for (GtkWidget *w : {name_label, info_label}) {
      GtkGesture *dbl = gtk_gesture_click_new();
      g_signal_connect(dbl, "pressed", G_CALLBACK(+[] (GtkGestureClick *, int n_press, double, double, gpointer d) {
                          if (n_press == 2) ask_rename_molecule(GPOINTER_TO_INT(d)); }), GINT_TO_POINTER(imol));
      gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(dbl));
   }

   if (!model) {
      // contour level and opacity, right on the map's row
      GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_widget_set_margin_top(controls, 3);
      auto *contour = new drag_number_t;
      contour->value = get_contour_level_in_sigma(imol);
      contour->lo = diff ? 0.5 : 0.1; contour->hi = diff ? 10.0 : 6.0;
      contour->per_pixel = 0.01; contour->decimals = 2;
      contour->prefix = diff ? "±" : ""; contour->suffix = " σ";
      contour->on_change = [imol] (double v) {
         set_contour_level_in_sigma(imol, v);
         for (const auto &r : map_rows) if (r.imol == imol) update_map_readout(r);
         graphics_draw(); };
      auto *opacity = new drag_number_t;
      opacity->value = 100.0 * get_solid_density_surface_opacity(imol);
      opacity->lo = 0; opacity->hi = 100; opacity->per_pixel = 0.5; opacity->decimals = 0; opacity->suffix = " %";
      opacity->on_change = [imol] (double v) { set_solid_density_surface_opacity(imol, v / 100.0); };
      GtkWidget *absolute = gtk_label_new("");
      gtk_widget_add_css_class(absolute, "coot-plus-readout");
      gtk_widget_set_hexpand(absolute, TRUE);
      gtk_label_set_xalign(GTK_LABEL(absolute), 0.0);
      gtk_box_append(GTK_BOX(controls), make_drag_number(contour, "Contour level in σ (same scale for every map)"));
      gtk_box_append(GTK_BOX(controls), absolute);
      gtk_box_append(GTK_BOX(controls), make_drag_number(opacity, "Opacity: 100 % is solid, lower lets you see through the map"));
      gtk_box_append(GTK_BOX(text), controls);
      map_rows.push_back(map_row_t{imol, contour, opacity, absolute});
      update_map_readout(map_rows.back());
   }

   GtkWidget *close = gtk_button_new_with_label("✕");
   gtk_widget_add_css_class(close, "flat");
   gtk_widget_add_css_class(close, "coot-plus-close");
   gtk_widget_set_focusable(close, FALSE);
   gtk_widget_set_valign(close, GTK_ALIGN_START);
   gtk_widget_set_tooltip_text(close, model ? "Close this model (asks first)" : "Close this map (asks first)");
   g_signal_connect(close, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer d) {
                       ask_close_molecule(GPOINTER_TO_INT(d)); }), GINT_TO_POINTER(imol));

   GtkWidget *swatch = make_colour_swatch(imol, model);
   gtk_widget_set_halign(swatch, GTK_ALIGN_CENTER);
   gtk_box_append(GTK_BOX(type_box), swatch);
   GtkWidget *menu = make_molecule_menu(imol, model);
   gtk_widget_set_valign(menu, GTK_ALIGN_START);

   gtk_box_append(GTK_BOX(row), shown);
   gtk_box_append(GTK_BOX(row), type_box);
   gtk_box_append(GTK_BOX(row), text);
   gtk_box_append(GTK_BOX(row), menu);
   gtk_box_append(GTK_BOX(row), close);
   return row;
}

void rebuild_molecules_section() {

   GtkWidget *child;
   while ((child = gtk_widget_get_first_child(molecules_box)) != nullptr)
      gtk_box_remove(GTK_BOX(molecules_box), child);
   molecule_rows.clear();
   map_rows.clear();
   map_radius_number = nullptr;

   // the model maps are compared with: the first displayed model with a cell
   int ref_model = -1;
   for (int imol=0; imol<graphics_n_molecules(); imol++)
      if (is_valid_model_molecule(imol) && mol_is_displayed(imol) && model_cell(imol).size() >= 6) { ref_model = imol; break; }
   std::vector<float> ref_cell = ref_model >= 0 ? model_cell(ref_model) : std::vector<float>();

   std::vector<int> models, maps;
   for (int imol=0; imol<graphics_n_molecules(); imol++) {
      if (is_valid_model_molecule(imol)) models.push_back(imol);
      else if (is_valid_map_molecule(imol)) maps.push_back(imol);
   }
   if (!models.empty()) {
      gtk_box_append(GTK_BOX(molecules_box), molecule_section_heading(models.size() == 1 ? "Model" : "Models"));
      for (int imol : models) gtk_box_append(GTK_BOX(molecules_box), make_molecule_row(imol, ref_model, ref_cell));
   }
   if (!maps.empty()) {
      gtk_box_append(GTK_BOX(molecules_box), molecule_section_heading(maps.size() == 1 ? "Map" : "Maps"));
      for (int imol : maps) gtk_box_append(GTK_BOX(molecules_box), make_molecule_row(imol, ref_model, ref_cell));
      GtkWidget *radius_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_widget_set_margin_start(radius_row, 4);
      GtkWidget *rl = gtk_label_new("Map radius");
      gtk_widget_add_css_class(rl, "coot-plus-dim");
      map_radius_number = new drag_number_t;
      map_radius_number->value = get_map_radius();
      map_radius_number->lo = 5; map_radius_number->hi = 60; map_radius_number->per_pixel = 0.2;
      map_radius_number->decimals = 0; map_radius_number->suffix = " Å";
      map_radius_number->on_change = [] (double v) { set_map_radius(v); };
      gtk_box_append(GTK_BOX(radius_row), rl);
      gtk_box_append(GTK_BOX(radius_row), make_drag_number(map_radius_number,
                     "How far around the centre the maps are drawn (keys [ and ])"));
      gtk_box_append(GTK_BOX(molecules_box), radius_row);
   }

   if (models.empty() && maps.empty()) {
      GtkWidget *l = gtk_label_new("Nothing open yet.\nLoad › Open Coordinates, or Auto Open MTZ for maps.");
      gtk_label_set_xalign(GTK_LABEL(l), 0.0);
      gtk_label_set_wrap(GTK_LABEL(l), TRUE);
      gtk_widget_add_css_class(l, "coot-plus-dim");
      gtk_box_append(GTK_BOX(molecules_box), l);
   } else {
      GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      GtkWidget *dm = gtk_button_new_with_label("Display Manager…");
      GtkWidget *all = gtk_button_new_with_label("Close all…");
      for (GtkWidget *bt : {dm, all}) {
         gtk_widget_add_css_class(bt, "flat");
         gtk_widget_add_css_class(bt, "coot-plus-link");
         gtk_widget_set_focusable(bt, FALSE);
      }
      gtk_widget_set_hexpand(dm, TRUE);
      gtk_widget_set_halign(dm, GTK_ALIGN_START);
      gtk_widget_set_tooltip_text(dm, "Every display option for every molecule: drawing style, colours, symmetry (F8)");
      gtk_widget_set_tooltip_text(all, "Close every model and map (asks first) - to start again");
      g_signal_connect(dm,  "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { open_display_manager(); }), nullptr);
      g_signal_connect(all, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { ask_close_everything(); }), nullptr);
      gtk_box_append(GTK_BOX(bottom), dm);
      gtk_box_append(GTK_BOX(bottom), all);
      gtk_box_append(GTK_BOX(molecules_box), bottom);
   }
}

void update_molecules_section() {
   std::string sig;
   for (int imol=0; imol<graphics_n_molecules(); imol++) {
      bool model = is_valid_model_molecule(imol), map = is_valid_map_molecule(imol);
      if (!model && !map) continue;
      sig += std::to_string(imol) + ":" + molecule_name(imol) + ":" + (model ? "m" : "x") +
             std::to_string(model ? mol_is_displayed(imol) : 0) + std::to_string(model ? mol_is_active(imol) : 0) +
             (model ? std::to_string(graphics_info_t::molecules[imol].atom_sel.n_selected_atoms) : "") + ";";
   }
   for (int imol=0; imol<graphics_n_molecules(); imol++)
      if (is_valid_model_molecule(imol) || is_valid_map_molecule(imol)) sig += display_name(imol) + ";";
   sig += "r" + std::to_string(imol_refinement_map());
   if (sig != molecules_signature) {
      molecules_signature = sig;
      updating_widgets = true;
      rebuild_molecules_section();
      updating_widgets = false;
      refresh_recent_and_groups(); // a file was probably just opened
      return;
   }
   updating_widgets = true;
   for (const auto &r : molecule_rows) {
      bool d = is_valid_model_molecule(r.imol) ? mol_is_displayed(r.imol) : map_is_displayed(r.imol);
      if (gtk_check_button_get_active(GTK_CHECK_BUTTON(r.shown)) != d)
         gtk_check_button_set_active(GTK_CHECK_BUTTON(r.shown), d);
   }
   updating_widgets = false;
}


// ------------------------------------------------------------------------------------
//          pick-mode banner: "now click an atom" modes get a banner + Esc
// ------------------------------------------------------------------------------------

struct pick_mode_t { const char *name; std::function<int()> get; const char *text; };

const std::vector<pick_mode_t> &pick_modes() {
   using g = graphics_info_t;
   static const std::vector<pick_mode_t> v = {
      {"range",      [] { return int(g::in_range_define); }, ""}, // text depends on the step (below)
      {"pepflip",    [] { return int(g::in_pepflip_define); },      "Flip Peptide: click an atom of the peptide"},
      {"rigid",      [] { return int(g::in_rigid_body_define); },   "Rigid Body Fit: click the first and last atoms of the range"},
      {"terminal",   [] { return int(g::in_terminal_residue_define); }, "Add Terminal Residue: click an atom of the terminal residue"},
      {"rottrans",   [] { return int(g::in_rot_trans_object_define); }, "Rotate/Translate: click an atom"},
      {"resinfo",    [] { return int(g::in_residue_info_define); }, "Residue Info: click an atom"},
      {"distance",   [] { return int(g::in_distance_define); },     "Distance: click two atoms"},
      {"angle",      [] { return int(g::in_angle_define); },        "Angle: click three atoms"},
      {"torsion",    [] { return int(g::in_torsion_define); },      "Torsion: click four atoms"},
      {"rotamer",    [] { return int(g::in_rotamer_define); },      "Rotamers: click an atom of the residue"},
      {"mutate",     [] { return int(g::in_mutate_define); },       "Mutate: click an atom of the residue"},
      {"mutatefit",  [] { return int(g::in_mutate_auto_fit_define); }, "Mutate & Auto-Fit: click an atom of the residue"},
      {"autofit",    [] { return int(g::in_auto_fit_define); },     "Auto-Fit Rotamer: click an atom of the residue"},
      {"dbmain",     [] { return int(g::in_db_main_define); },      "DB Main-chain: click the first and last atoms"},
      {"phipsi",     [] { return int(g::in_edit_phi_psi_define); }, "Edit Phi/Psi: click an atom of the residue"},
      {"altconf",    [] { return int(g::in_add_alt_conf_define); }, "Add Alt Conf: click an atom of the residue"},
      {"savesymm",   [] { return int(g::in_save_symmetry_define); }, "Save Symmetry Coordinates: click a symmetry atom"},
      {"cistrans",   [] { return int(g::in_cis_trans_convert_define); }, "cis ↔ trans: click an atom of the peptide"},
      {"flip180",    [] { return int(g::in_180_degree_flip_define); }, "Side-chain 180° Flip: click an atom of the residue"},
      {"reverse",    [] { return int(g::in_reverse_direction_define); }, "Reverse Direction: click an atom of the fragment"},
      {"dyndist",    [] { return int(g::in_dynamic_distance_define); }, "Dynamic Distance: click two atoms"},
      {"torsiongen", [] { return int(g::in_torsion_general_define); }, "Torsion: click four atoms"},
      {"multitors",  [] { return int(g::in_multi_residue_torsion_define); }, "Multi-residue Torsion: click the atoms"},
      {"editchi",    [] { return int(g::in_edit_chi_angles_define); }, "Edit Chi Angles: click an atom of the residue"},
      {"user",       [] { return int(g::in_user_defined_define); }, "Click the atoms the script asked for"},
      {"delatom",    [] { return int(g::delete_item_atom); },       "Delete: click an atom to delete it (Ctrl-click to keep deleting)"},
      {"delwater",   [] { return int(g::delete_item_water); },      "Delete: click a water to delete it (Ctrl-click to keep deleting)"}};
   return v;
}

GtkWidget *mode_banner = nullptr;
GtkWidget *mode_banner_label = nullptr;
std::string current_mode;

std::string active_pick_mode_text() {
   for (const auto &m : pick_modes()) {
      int v = m.get();
      if (!v) continue;
      current_mode = m.name;
      if (std::string(m.name) == "range")
         return v == 1 ? "Pick Residue Range: click the first atom of the range"
                       : "Pick Residue Range: now click the last atom";
      return m.text;
   }
   current_mode.clear();
   return "";
}

void cancel_pick_modes() {
   graphics_info_t g;
   g.clear_pending_picks();
   graphics_info_t::delete_item_atom = 0;
   graphics_info_t::delete_item_water = 0;
   if (GtkWidget *rp = widget_from_builder("select_residue_range_toggle_button"))
      if (GTK_IS_TOGGLE_BUTTON(rp)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rp), FALSE);
   graphics_info_t::add_status_bar_text("Cancelled");
   if (mode_banner) gtk_widget_set_visible(mode_banner, FALSE);
}

gboolean mode_banner_tick(gpointer) {
   if (!mode_banner) return G_SOURCE_REMOVE;
   ignore_history_scope ignore;
   std::string t = active_pick_mode_text();
   bool show = !t.empty();
   if (show) gtk_label_set_text(GTK_LABEL(mode_banner_label), t.c_str());
   if (gtk_widget_get_visible(mode_banner) != show) gtk_widget_set_visible(mode_banner, show);
   return G_SOURCE_CONTINUE;
}

GtkWidget *make_mode_banner() {
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
   gtk_widget_add_css_class(box, "coot-plus-mode-banner");
   gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
   gtk_widget_set_valign(box, GTK_ALIGN_START);
   gtk_widget_set_margin_top(box, 12);
   GtkWidget *dot = gtk_label_new("●");
   gtk_widget_add_css_class(dot, "coot-plus-mode-dot");
   mode_banner_label = gtk_label_new("");
   GtkWidget *cancel = gtk_button_new_with_label("Cancel  (Esc)");
   gtk_widget_set_focusable(cancel, FALSE);
   gtk_widget_add_css_class(cancel, "coot-plus-mode-cancel");
   g_signal_connect(cancel, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { cancel_pick_modes(); }), nullptr);
   gtk_box_append(GTK_BOX(box), dot);
   gtk_box_append(GTK_BOX(box), mode_banner_label);
   gtk_box_append(GTK_BOX(box), cancel);
   gtk_widget_set_visible(box, FALSE);
   gtk_widget_set_can_target(dot, FALSE);
   mode_banner = box;
   g_timeout_add(150, mode_banner_tick, nullptr);
   return box;
}

void add_escape_cancels_pick_modes(GtkWidget *window) {
   GtkEventController *keys = gtk_event_controller_key_new();
   gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
   g_signal_connect(keys, "key-pressed", G_CALLBACK(+[] (GtkEventControllerKey *, guint keyval, guint, GdkModifierType,
                                                          gpointer) -> gboolean {
                       if (keyval != GDK_KEY_Escape) return FALSE;
                       if (active_pick_mode_text().empty()) return FALSE; // let Coot have Esc (reject moving atoms)
                       cancel_pick_modes();
                       return TRUE;
                    }), nullptr);
   gtk_widget_add_controller(window, keys);
}

// ------------------------------------------------------------------------------------
//          message strip: errors and warnings visible in the window
// ------------------------------------------------------------------------------------

GtkWidget *message_strip = nullptr;
GtkWidget *message_icon = nullptr;
GtkWidget *message_text = nullptr;
GtkWidget *message_hint = nullptr;
guint message_timeout = 0;
unsigned int logger_seen = 0;
std::string last_message;
gint64 ui_ready_time = 0;

void hide_message_strip() {
   if (message_strip) gtk_revealer_set_reveal_child(GTK_REVEALER(message_strip), FALSE);
   if (message_timeout) { g_source_remove(message_timeout); message_timeout = 0; }
}

void show_message(const std::string &kind, const std::string &text, const std::string &hint) {
   if (!message_strip) return;
   if (text == last_message && gtk_revealer_get_reveal_child(GTK_REVEALER(message_strip))) return;
   last_message = text;
   GtkWidget *bar = gtk_revealer_get_child(GTK_REVEALER(message_strip));
   for (const char *k : {"error", "warning", "info"})
      gtk_widget_remove_css_class(bar, (std::string("coot-plus-msg-") + k).c_str());
   gtk_widget_add_css_class(bar, ("coot-plus-msg-" + kind).c_str());
   gtk_label_set_text(GTK_LABEL(message_icon), kind == "error" ? "✖" : (kind == "warning" ? "⚠" : "ℹ"));
   gtk_label_set_text(GTK_LABEL(message_text), text.c_str());
   gtk_label_set_text(GTK_LABEL(message_hint), hint.c_str());
   gtk_widget_set_visible(message_hint, !hint.empty());
   gtk_revealer_set_reveal_child(GTK_REVEALER(message_strip), TRUE);
   if (message_timeout) g_source_remove(message_timeout);
   message_timeout = g_timeout_add_seconds(kind == "error" ? 20 : 12, +[] (gpointer) -> gboolean {
                                              message_timeout = 0; hide_message_strip(); return G_SOURCE_REMOVE; }, nullptr);
}

// new WARNING/ERROR entries from Coot's logger
gboolean watch_logger(gpointer) {
   std::vector<logging::log_item> items = logger.get_log_history_from(logger_seen);
   logger_seen += items.size();
   if (g_get_monotonic_time() < ui_ready_time) return G_SOURCE_CONTINUE; // ignore start-up chatter
   for (const auto &li : items) {
      if (li.type != log_t::ERROR && li.type != log_t::WARNING) continue;
      std::string msg = li.message;
      if (!li.function_name.empty() && msg.empty()) msg = li.function_name.get_name();
      if (msg.empty()) continue;
      if (msg.length() > 220) msg = msg.substr(0, 217) + "...";
      show_message(li.type == log_t::ERROR ? "error" : "warning", msg, "Full details: Log (header)");
   }
   return G_SOURCE_CONTINUE;
}

GtkWidget *make_message_strip() {
   GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
   gtk_widget_add_css_class(bar, "coot-plus-message");
   message_icon = gtk_label_new("");
   gtk_widget_add_css_class(message_icon, "coot-plus-msg-icon");
   GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
   gtk_widget_set_hexpand(texts, TRUE);
   message_text = gtk_label_new("");
   message_hint = gtk_label_new("");
   for (GtkWidget *l : {message_text, message_hint}) {
      gtk_label_set_xalign(GTK_LABEL(l), 0.0);
      gtk_label_set_wrap(GTK_LABEL(l), TRUE);
      gtk_label_set_selectable(GTK_LABEL(l), TRUE);
      gtk_box_append(GTK_BOX(texts), l);
   }
   gtk_widget_add_css_class(message_hint, "coot-plus-dim");
   GtkWidget *close = gtk_button_new_with_label("✕");
   gtk_widget_add_css_class(close, "flat");
   gtk_widget_set_focusable(close, FALSE);
   gtk_widget_set_valign(close, GTK_ALIGN_CENTER);
   g_signal_connect(close, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { hide_message_strip(); }), nullptr);
   gtk_box_append(GTK_BOX(bar), message_icon);
   gtk_box_append(GTK_BOX(bar), texts);
   gtk_box_append(GTK_BOX(bar), close);
   GtkWidget *rev = gtk_revealer_new();
   gtk_revealer_set_transition_type(GTK_REVEALER(rev), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
   gtk_revealer_set_child(GTK_REVEALER(rev), bar);
   gtk_revealer_set_reveal_child(GTK_REVEALER(rev), FALSE);
   message_strip = rev;
   logger_seen = logger.get_log_history_from(0).size();
   ui_ready_time = g_get_monotonic_time() + 4 * G_USEC_PER_SEC;
   g_timeout_add(500, watch_logger, nullptr);
   return rev;
}

// ------------------------------------------------------------------------------------
//          command log: every action as a replayable Python line
// ------------------------------------------------------------------------------------

std::vector<std::string> command_log;
size_t history_seen = 0;
GtkWidget *command_log_window = nullptr;
GtkTextBuffer *command_log_buffer = nullptr;

void append_to_command_log_view(const std::string &line) {
   if (!command_log_buffer) return;
   GtkTextIter end;
   gtk_text_buffer_get_end_iter(command_log_buffer, &end);
   gtk_text_buffer_insert(command_log_buffer, &end, (line + "\n").c_str(), -1);
}

void log_line(const std::string &line) {
   if (!command_log.empty() && command_log.back() == line) return;
   command_log.push_back(line);
   append_to_command_log_view(line);
}

// commands Coot records in its history (these are what "Save State"/history scripts replay)
bool is_query_command(const std::string &name) {
   // read-only functions: not worth replaying
   static const std::vector<std::string> prefixes = {"get-", "get_", "is-", "is_", "mol-is-", "mol_is_", "map-is-", "map_is_",
                                                       "molecule-name", "molecule_name", "graphics-molecule-bond-type",
                                                       "graphics_molecule_bond_type", "graphics-n-molecules",
                                                       "graphics_n_molecules", "imol-refinement-map", "imol_refinement_map",
                                                       "scroll-wheel-map", "scroll_wheel_map", "valid-", "valid_"};
   for (const auto &p : prefixes) if (name.rfind(p, 0) == 0) return true;
   return false;
}

gboolean watch_history(gpointer) {
   const auto &h = graphics_info_t::history_list.history_strings;
   for (; history_seen < h.size(); history_seen++) {
      bool ignored = false;
      for (const auto &r : history_ignored) if (history_seen >= r.first && history_seen < r.second) ignored = true;
      if (ignored || h[history_seen].empty()) continue;
      const std::string &name = (h[history_seen][0] == "coot" && h[history_seen].size() > 1) ? h[history_seen][1] : h[history_seen][0];
      if (is_query_command(name)) continue;
      std::string py = languagize_command(h[history_seen]);
      if (py.empty()) continue;
      if (py.rfind("coot.", 0) != 0 && py.find('(') != std::string::npos && py.find('.') > py.find('('))
         py = "coot." + py;
      log_line(py);
   }
   // forget ranges that are fully behind us
   history_ignored.erase(std::remove_if(history_ignored.begin(), history_ignored.end(),
                                        [] (const std::pair<size_t, size_t> &r) { return r.second <= history_seen; }),
                         history_ignored.end());
   return G_SOURCE_CONTINUE;
}

void save_command_log_as(GtkWindow *parent) {
   GtkFileDialog *fd = gtk_file_dialog_new();
   gtk_file_dialog_set_title(fd, "Save Command Log as a Python Script");
   gtk_file_dialog_set_initial_name(fd, "coot-commands.py");
   gtk_file_dialog_save(fd, parent, nullptr, +[] (GObject *src, GAsyncResult *res, gpointer) {
                           GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, nullptr);
                           if (!f) return;
                           std::string text = "# Coot+ command log - run with Calculate > Run Script...\nimport coot\n\n";
                           for (const auto &l : command_log) text += l + "\n";
                           g_file_replace_contents(f, text.c_str(), text.size(), nullptr, FALSE, G_FILE_CREATE_NONE,
                                                   nullptr, nullptr, nullptr);
                           char *path = g_file_get_path(f);
                           graphics_info_t::add_status_bar_text(std::string("Command log saved to ") + (path ? path : "file"));
                           g_free(path);
                           g_object_unref(f);
                        }, nullptr);
   g_object_unref(fd);
}

void show_command_log() {
   if (!command_log_window) {
      GtkWidget *w = gtk_window_new();
      gtk_window_set_title(GTK_WINDOW(w), "Coot+ Command Log");
      gtk_window_set_default_size(GTK_WINDOW(w), 640, 420);
      gtk_window_set_hide_on_close(GTK_WINDOW(w), TRUE);
      if (GtkWidget *mw = graphics_info_t::get_main_window()) gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(mw));
      GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
      gtk_widget_set_margin_start(box, 10); gtk_widget_set_margin_end(box, 10);
      gtk_widget_set_margin_top(box, 10);   gtk_widget_set_margin_bottom(box, 10);
      GtkWidget *intro = gtk_label_new("What you did, as Python. Lines starting with # are menu or toolbar actions "
                                       "that Coot doesn't record as a command. Copy lines into the Python console, "
                                       "or save them all as a script.");
      gtk_label_set_wrap(GTK_LABEL(intro), TRUE);
      gtk_label_set_xalign(GTK_LABEL(intro), 0.0);
      gtk_widget_add_css_class(intro, "coot-plus-dim");
      GtkWidget *tv = gtk_text_view_new();
      gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
      gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
      gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 6);
      command_log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
      for (const auto &l : command_log) append_to_command_log_view(l);
      GtkWidget *sw = gtk_scrolled_window_new();
      gtk_widget_set_vexpand(sw, TRUE);
      gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);
      GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      GtkWidget *copy = gtk_button_new_with_label("Copy All");
      GtkWidget *save = gtk_button_new_with_label("Save as Script…");
      GtkWidget *clear = gtk_button_new_with_label("Clear");
      g_signal_connect(copy, "clicked", G_CALLBACK(+[] (GtkButton *b, gpointer) {
                          std::string text;
                          for (const auto &l : command_log) text += l + "\n";
                          gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(b)), text.c_str());
                          graphics_info_t::add_status_bar_text("Command log copied");
                       }), nullptr);
      g_signal_connect(save, "clicked", G_CALLBACK(+[] (GtkButton *b, gpointer) {
                          save_command_log_as(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b)))); }), nullptr);
      g_signal_connect(clear, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) {
                          command_log.clear();
                          if (command_log_buffer) gtk_text_buffer_set_text(command_log_buffer, "", -1); }), nullptr);
      gtk_box_append(GTK_BOX(buttons), copy);
      gtk_box_append(GTK_BOX(buttons), save);
      gtk_box_append(GTK_BOX(buttons), clear);
      gtk_box_append(GTK_BOX(box), intro);
      gtk_box_append(GTK_BOX(box), sw);
      gtk_box_append(GTK_BOX(box), buttons);
      gtk_window_set_child(GTK_WINDOW(w), box);
      command_log_window = w;
   }
   gtk_window_present(GTK_WINDOW(command_log_window));
}

// ------------------------------------------------------------------------------------
//          favourites: right-click a tool to pin it
// ------------------------------------------------------------------------------------


std::string command_key(const command_t &c) { return c.label + "|" + c.path + "|" + c.action + "|" + c.target; }

void save_favourites() {
   std::string fn = coot_plus_settings_file();
   GKeyFile *kf = g_key_file_new();
   g_key_file_load_from_file(kf, fn.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
   std::vector<const char *> v;
   for (const auto &f : favourites) v.push_back(f.c_str());
   g_key_file_set_string_list(kf, "interface", "favourites", v.data(), v.size());
   gchar *dir = g_path_get_dirname(fn.c_str());
   g_mkdir_with_parents(dir, 0755);
   g_free(dir);
   g_key_file_save_to_file(kf, fn.c_str(), nullptr);
   g_key_file_free(kf);
}

void load_favourites() {
   favourites.clear();
   GKeyFile *kf = g_key_file_new();
   if (g_key_file_load_from_file(kf, coot_plus_settings_file().c_str(), G_KEY_FILE_NONE, nullptr)) {
      gsize n = 0;
      gchar **l = g_key_file_get_string_list(kf, "interface", "favourites", &n, nullptr);
      for (gsize i=0; l && i<n; i++) favourites.push_back(l[i]);
      g_strfreev(l);
   }
   g_key_file_free(kf);
}


void rebuild_favourites() {
   if (!favourites_box) return;
   GtkWidget *child;
   while ((child = gtk_widget_get_first_child(favourites_box)) != nullptr)
      gtk_box_remove(GTK_BOX(favourites_box), child);
   int n = 0;
   for (const auto &key : favourites)
      for (const auto &c : commands)
         if (command_key(c) == key) { gtk_box_append(GTK_BOX(favourites_box), make_tool_button(c, "", true)); n++; break; }
   if (n == 0) {
      GtkWidget *l = gtk_label_new("Right-click any tool and choose “Add to Favourites” to pin it here.");
      gtk_label_set_wrap(GTK_LABEL(l), TRUE);
      gtk_label_set_max_width_chars(GTK_LABEL(l), 24);
      gtk_label_set_xalign(GTK_LABEL(l), 0.0);
      gtk_widget_add_css_class(l, "coot-plus-page-hint");
      gtk_box_append(GTK_BOX(favourites_box), l);
   }
}

void toggle_favourite(const command_t &c) {
   std::string key = command_key(c);
   auto it = std::find(favourites.begin(), favourites.end(), key);
   if (it == favourites.end()) {
      favourites.push_back(key);
      graphics_info_t::add_status_bar_text("Added " + c.label + " to Favourites");
   } else {
      favourites.erase(it);
      graphics_info_t::add_status_bar_text("Removed " + c.label + " from Favourites");
   }
   save_favourites();
   // rebuilding destroys the button that was right-clicked - do it after this event
   g_idle_add(+[] (gpointer) -> gboolean { rebuild_favourites(); return G_SOURCE_REMOVE; }, nullptr);
}

// an entry of a customised Common list for a command: "cmd:<command key>|<label shown>"
std::string common_entry_for(const command_t &c, const std::string &label) { return "cmd:" + command_key(c) + "\x1f" + label; }

// does a Common-list entry stand for this command?
bool common_entry_is(const std::string &entry, const std::string &key) {
   if (entry.rfind("cmd:", 0) == 0) return entry.substr(4, entry.find('\x1f') - 4) == key;
   if (entry.rfind("special:", 0) == 0 || entry.rfind("section:", 0) == 0 || entry.rfind("menu:", 0) == 0) return false;
   const command_t *cc = resolve_tool(entry);
   return cc && command_key(*cc) == key;
}

void attach_favourite_menu(GtkWidget *button, const command_t &c, const std::string &shown_label) {
   GtkGesture *rc = gtk_gesture_click_new();
   gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), GDK_BUTTON_SECONDARY);
   command_t *copy = new command_t(c);
   g_object_set_data_full(G_OBJECT(rc), "cmd", copy, +[] (gpointer d) { delete static_cast<command_t *>(d); });
   g_object_set_data_full(G_OBJECT(rc), "task", g_strdup(building_task.c_str()), g_free);
   g_object_set_data_full(G_OBJECT(rc), "shown", g_strdup(shown_label.c_str()), g_free);
   g_signal_connect(rc, "pressed", G_CALLBACK(+[] (GtkGestureClick *g, int, double, double, gpointer) {
                       command_t *c = static_cast<command_t *>(g_object_get_data(G_OBJECT(g), "cmd"));
                       GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
                       bool fav = std::find(favourites.begin(), favourites.end(), command_key(*c)) != favourites.end();
                       GtkWidget *pop = gtk_popover_new();
                       GtkWidget *b = gtk_button_new_with_label(fav ? "★  Remove from Favourites" : "☆  Add to Favourites");
                       gtk_widget_add_css_class(b, "flat");
                       g_object_set_data(G_OBJECT(b), "cmd", c);
                       g_object_set_data(G_OBJECT(b), "pop", pop);
                       g_signal_connect(b, "clicked", G_CALLBACK(+[] (GtkButton *btn, gpointer) {
                                           command_t *c = static_cast<command_t *>(g_object_get_data(G_OBJECT(btn), "cmd"));
                                           GtkWidget *p = GTK_WIDGET(g_object_get_data(G_OBJECT(btn), "pop"));
                                           command_t copy = *c;
                                           gtk_popover_popdown(GTK_POPOVER(p));
                                           toggle_favourite(copy);
                                        }), nullptr);
                       GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
                       gtk_box_append(GTK_BOX(pbox), b);
                       std::string task = static_cast<const char *>(g_object_get_data(G_OBJECT(g), "task"));
                       if (!task.empty() && task != "Favourites") {
                          std::vector<std::string> cur = custom_common(task);
                          bool customised = !cur.empty();
                          if (!customised) cur = common_tools().at(task); // start from what the Common list shows now
                          std::string key = command_key(*c);
                          bool in = false;
                          for (const auto &e : cur) if (common_entry_is(e, key)) in = true;
                          std::string shown = static_cast<const char *>(g_object_get_data(G_OBJECT(g), "shown"));
                          GtkWidget *cb = gtk_button_new_with_label(in ? ("Remove from " + task + " Common list").c_str()
                                                                       : ("Add to " + task + " Common list").c_str());
                          gtk_widget_add_css_class(cb, "flat");
                          common_menu_t *cm = new common_menu_t;
                          cm->task = task; cm->key = key; cm->cur = cur; cm->in = in; cm->pop = pop;
                          cm->entry = common_entry_for(*c, shown);
                          g_object_set_data_full(G_OBJECT(cb), "cm", cm, +[] (gpointer d) { delete static_cast<common_menu_t *>(d); });
                          g_signal_connect(cb, "clicked", G_CALLBACK(+[] (GtkButton *btn, gpointer) {
                                              common_menu_t *cm = static_cast<common_menu_t *>(g_object_get_data(G_OBJECT(btn), "cm"));
                                              std::vector<std::string> v = cm->cur;
                                              if (cm->in) {
                                                 std::vector<std::string> keep;
                                                 for (const auto &e : v) if (!common_entry_is(e, cm->key)) keep.push_back(e);
                                                 v = keep;
                                              } else {
                                                 v.push_back(cm->entry);
                                              }
                                              save_custom_common(cm->task, v, false);
                                              gtk_popover_popdown(GTK_POPOVER(cm->pop));
                                              g_idle_add(+[] (gpointer) -> gboolean { rebuild_sidebar(); return G_SOURCE_REMOVE; }, nullptr);
                                           }), nullptr);
                          gtk_box_append(GTK_BOX(pbox), cb);
                          if (customised) {
                             GtkWidget *reset = gtk_button_new_with_label(("Reset " + task + " Common list").c_str());
                             gtk_widget_add_css_class(reset, "flat");
                             g_object_set_data_full(G_OBJECT(reset), "task", g_strdup(task.c_str()), g_free);
                             g_object_set_data(G_OBJECT(reset), "pop", pop);
                             g_signal_connect(reset, "clicked", G_CALLBACK(+[] (GtkButton *btn, gpointer) {
                                                 save_custom_common(static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "task")), {}, true);
                                                 gtk_popover_popdown(GTK_POPOVER(g_object_get_data(G_OBJECT(btn), "pop")));
                                                 g_idle_add(+[] (gpointer) -> gboolean { rebuild_sidebar(); return G_SOURCE_REMOVE; }, nullptr);
                                              }), nullptr);
                             gtk_box_append(GTK_BOX(pbox), reset);
                          }
                       }
                       gtk_popover_set_child(GTK_POPOVER(pop), pbox);
                       gtk_widget_set_parent(pop, w);
                       g_signal_connect(pop, "closed", G_CALLBACK(+[] (GtkPopover *p, gpointer) {
                                           g_idle_add(+[] (gpointer p) -> gboolean { gtk_widget_unparent(GTK_WIDGET(p));
                                                                                    return G_SOURCE_REMOVE; }, p); }), nullptr);
                       gtk_popover_popup(GTK_POPOVER(pop));
                    }), nullptr);
   gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(rc));
}

// ------------------------------------------------------------------------------------
//          Preferences: a Coot+ page (label size, mouse and trackpad)
// ------------------------------------------------------------------------------------

GtkWidget *pref_row(const char *title, const char *tip, GtkWidget *control) {
   GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
   GtkWidget *l = gtk_label_new(title);
   gtk_label_set_xalign(GTK_LABEL(l), 0.0);
   gtk_widget_set_hexpand(l, TRUE);
   gtk_widget_set_tooltip_text(row, tip);
   gtk_box_append(GTK_BOX(row), l);
   gtk_box_append(GTK_BOX(row), control);
   return row;
}

GtkWidget *pref_choice(const std::vector<std::string> &labels, int active, void (*on_change)(int)) {
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
   gtk_widget_add_css_class(box, "linked");
   GtkWidget *first = nullptr;
   for (unsigned int i=0; i<labels.size(); i++) {
      GtkWidget *b = gtk_toggle_button_new_with_label(labels[i].c_str());
      if (first) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(first)); else first = b;
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), int(i) == active);
      g_object_set_data(G_OBJECT(b), "idx", GINT_TO_POINTER(i));
      g_object_set_data(G_OBJECT(b), "fn", reinterpret_cast<gpointer>(on_change));
      g_signal_connect(b, "toggled", G_CALLBACK(+[] (GtkToggleButton *t, gpointer) {
                          if (!gtk_toggle_button_get_active(t)) return;
                          auto fn = reinterpret_cast<void (*)(int)>(g_object_get_data(G_OBJECT(t), "fn"));
                          fn(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "idx")));
                       }), nullptr);
      gtk_box_append(GTK_BOX(box), b);
   }
   return box;
}

void add_preferences_page() {
   GtkWidget *nb = widget_from_preferences_builder("preferences_notebook");
   if (!nb || !GTK_IS_NOTEBOOK(nb)) return;
   GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
   gtk_widget_set_margin_start(page, 16); gtk_widget_set_margin_end(page, 16);
   gtk_widget_set_margin_top(page, 16);   gtk_widget_set_margin_bottom(page, 16);
   gtk_box_append(GTK_BOX(page), section_label("Atom labels"));
   gtk_box_append(GTK_BOX(page),
      pref_row("Label size", "Size of the atom labels in the 3D view (also A−/A+ in the inspector)",
               pref_choice({"Small", "Medium", "Large", "Larger", "Largest"}, std::max(0, get_font_size() - 1),
                           [] (int i) { set_font_size(i + 1);
                                        preferences_internal_change_value_int(PREFERENCES_FONT_SIZE, i + 1);
                                        graphics_draw(); })));
   gtk_box_append(GTK_BOX(page), section_label("Mouse and trackpad"));
   gtk_box_append(GTK_BOX(page),
      pref_row("Mouse scroll wheel", "Hold Shift while scrolling to do the other one",
               pref_choice({"Zoom", "Map contour"}, get_scroll_wheel_zooms() ? 0 : 1,
                           [] (int i) { set_scroll_mode(i == 0); })));
   gtk_box_append(GTK_BOX(page),
      pref_row("Trackpad two-finger drag", "Rotate: two fingers turn the molecule (pinch zooms, Shift + two fingers changes the contour).\n"
                                           "Scroll: two fingers act like the mouse scroll wheel.",
               pref_choice({"Rotate", "Scroll"}, get_trackpad_drag_rotates() ? 0 : 1,
                           [] (int i) { set_trackpad_drag_rotates(i == 0);
                                        save_coot_plus_setting_bool("trackpad-drag-rotates", i == 0); })));
   gtk_box_append(GTK_BOX(page),
      pref_row("Double-click an atom", "Option-click always centres on an atom",
               pref_choice({"Centre on it", "Label it"}, get_double_click_centres() ? 0 : 1,
                           [] (int i) { set_double_click_centres(i == 0);
                                        save_coot_plus_setting_bool("double-click-centres", i == 0); })));
   gtk_box_append(GTK_BOX(page), section_label("Appearance"));
   {
      std::string th = load_coot_plus_setting_string("theme", "dark");
      gtk_box_append(GTK_BOX(page), pref_row("Theme", "Light or dark windows and panels",
         pref_choice({"Dark", "Light", "System"}, th == "light" ? 1 : (th == "system" ? 2 : 0),
                     [] (int i) { set_theme(i == 0 ? "dark" : (i == 1 ? "light" : "system")); })));
      std::string ac = load_coot_plus_setting_string("accent", "#3584e4");
      std::vector<std::string> names;
      int active = 0;
      for (unsigned int i=0; i<accent_colours().size(); i++) {
         names.push_back(accent_colours()[i].first);
         if (accent_colours()[i].second == ac) active = i;
      }
      gtk_box_append(GTK_BOX(page), pref_row("Accent colour", "Colour of the selected task, buttons and highlights",
         pref_choice(names, active, [] (int i) { set_accent(accent_colours()[i].second); })));
      gtk_box_append(GTK_BOX(page), pref_row("3D background", "Background of the 3D view",
         pref_choice({"Black", "Dark grey", "White"}, 0, [] (int i) {
            const char *a[] = {"background_black_action", "background_dark_grey_action", "background_white_action"};
            g_action_group_activate_action(G_ACTION_GROUP(coot_plus_app), a[i], nullptr); })));
   }
   gtk_box_append(GTK_BOX(page), section_label("Panels"));
   gtk_box_append(GTK_BOX(page),
      pref_row("Task sidebar", "Compact: narrower, without shortcut chips, page hints and the help box",
               pref_choice({"Full", "Compact"}, compact_sidebar ? 1 : 0, [] (int i) { set_compact_sidebar(i == 1); })));
   gtk_box_append(GTK_BOX(page),
      pref_row("Classic build toolbar", "Coot's original column of build buttons on the right",
               pref_choice({"Hidden", "Shown"}, classic_toolbar && gtk_widget_get_visible(classic_toolbar) ? 1 : 0,
                           [] (int i) { set_classic_toolbar_visible(i == 1); })));
   gtk_notebook_append_page(GTK_NOTEBOOK(nb), page, gtk_label_new("Coot+"));
}

// ------------------------------------------------------------------------------------
//          header "more" menu
// ------------------------------------------------------------------------------------

GtkWidget *make_more_menu() {
   GtkWidget *mb = gtk_menu_button_new();
   gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(mb), "coot-plus-more-symbolic");
   gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(mb), FALSE);
   gtk_widget_set_focusable(mb, FALSE);
   gtk_widget_set_valign(mb, GTK_ALIGN_CENTER);
   gtk_widget_set_tooltip_text(mb, "More: command log, preferences, keyboard shortcuts");
   GtkWidget *pop = gtk_popover_new();
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
   add_menu_entry(box, pop, "Python Console…", "A console with history, multi-line input and completion of coot functions",
                  [] (int) { show_python_console(); }, 0);
   add_menu_entry(box, pop, "Compact / Full Sidebar", nullptr, [] (int) { set_compact_sidebar(!compact_sidebar); }, 0);
   add_menu_entry(box, pop, "Command Log…", "Everything you did, as Python you can copy or save as a script",
                  [] (int) { show_command_log(); }, 0);
   add_menu_entry(box, pop, "Preferences…", "Coot preferences - the Coot+ page has label size and mouse/trackpad settings",
                  [] (int) { g_action_group_activate_action(G_ACTION_GROUP(coot_plus_app), "show_preferences_action", nullptr); }, 0);
   add_menu_entry(box, pop, "Keyboard Shortcuts…", nullptr,
                  [] (int) { g_action_group_activate_action(G_ACTION_GROUP(coot_plus_app), "coot_shortcuts_action", nullptr); }, 0);
   add_menu_entry(box, pop, "Search Commands…  (⌘K)", nullptr, [] (int) { coot_plus_show_command_search(); }, 0);
   gtk_popover_set_child(GTK_POPOVER(pop), box);
   gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), pop);
   return mb;
}


// ------------------------------------------------------------------------------------
//          small helpers: menu entries with any callback, name dialog, choices
// ------------------------------------------------------------------------------------

void add_fn_entry(GtkWidget *box, GtkWidget *popover, const std::string &label, const char *tooltip,
                  std::function<void()> f) {
   GtkWidget *b = gtk_button_new_with_label(label.c_str());
   gtk_widget_add_css_class(b, "flat");
   gtk_widget_add_css_class(b, "coot-plus-menu-entry");
   gtk_widget_set_focusable(b, FALSE);
   if (tooltip) gtk_widget_set_tooltip_text(b, tooltip);
   GtkWidget *l = gtk_button_get_child(GTK_BUTTON(b));
   if (GTK_IS_LABEL(l)) gtk_label_set_xalign(GTK_LABEL(l), 0.0);
   g_object_set_data(G_OBJECT(b), "popover", popover);
   g_signal_connect_data(b, "clicked", G_CALLBACK(+[] (GtkButton *btn, gpointer d) {
                            GtkWidget *pop = GTK_WIDGET(g_object_get_data(G_OBJECT(btn), "popover"));
                            if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
                            std::function<void()> fn = *static_cast<std::function<void()> *>(d); // copy: rows may be rebuilt
                            fn();
                            give_focus_back_to_graphics();
                         }), new std::function<void()>(f),
                         +[] (gpointer d, GClosure *) { delete static_cast<std::function<void()> *>(d); }, GConnectFlags(0));
   gtk_box_append(GTK_BOX(box), b);
}

void add_menu_heading(GtkWidget *box, const std::string &text) {
   GtkWidget *l = section_label(text);
   gtk_widget_set_margin_start(l, 12);
   gtk_widget_set_margin_top(l, 8);
   gtk_box_append(GTK_BOX(box), l);
}

// a small window asking for a name; cb is called with the name on Save
void ask_name(const std::string &title, const std::string &prompt, const std::string &initial,
              std::function<void(const std::string &)> cb, bool allow_empty) {
   GtkWidget *w = gtk_window_new();
   g_object_set_data(G_OBJECT(w), "allow-empty", GINT_TO_POINTER(allow_empty ? 1 : 0));
   gtk_window_set_title(GTK_WINDOW(w), title.c_str());
   gtk_window_set_modal(GTK_WINDOW(w), TRUE);
   gtk_window_set_resizable(GTK_WINDOW(w), FALSE);
   if (GtkWidget *mw = graphics_info_t::get_main_window()) gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(mw));
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
   gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
   gtk_widget_set_margin_top(box, 14);   gtk_widget_set_margin_bottom(box, 14);
   GtkWidget *l = gtk_label_new(prompt.c_str());
   gtk_label_set_xalign(GTK_LABEL(l), 0.0);
   GtkWidget *e = gtk_entry_new();
   gtk_editable_set_text(GTK_EDITABLE(e), initial.c_str());
   gtk_editable_set_width_chars(GTK_EDITABLE(e), 32);
   GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_widget_set_halign(row, GTK_ALIGN_END);
   GtkWidget *cancel = gtk_button_new_with_label("Cancel");
   GtkWidget *ok = gtk_button_new_with_label("Save");
   gtk_widget_add_css_class(ok, "suggested-action");
   gtk_box_append(GTK_BOX(row), cancel);
   gtk_box_append(GTK_BOX(row), ok);
   gtk_box_append(GTK_BOX(box), l);
   gtk_box_append(GTK_BOX(box), e);
   gtk_box_append(GTK_BOX(box), row);
   gtk_window_set_child(GTK_WINDOW(w), box);
   auto *fn = new std::function<void(const std::string &)>(cb);
   g_object_set_data_full(G_OBJECT(w), "cb", fn, +[] (gpointer d) { delete static_cast<std::function<void(const std::string &)> *>(d); });
   g_object_set_data(G_OBJECT(w), "entry", e);
   auto do_ok = +[] (GtkWidget *any, gpointer win) {
      GtkWidget *w = GTK_WIDGET(win);
      std::string t = gtk_editable_get_text(GTK_EDITABLE(g_object_get_data(G_OBJECT(w), "entry")));
      while (!t.empty() && t.back() == ' ') t.pop_back();
      while (!t.empty() && t.front() == ' ') t.erase(0, 1);
      if (t.empty() && !GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "allow-empty"))) return;
      auto *f = static_cast<std::function<void(const std::string &)> *>(g_object_get_data(G_OBJECT(w), "cb"));
      std::function<void(const std::string &)> copy = *f;
      gtk_window_destroy(GTK_WINDOW(w));
      copy(t);
   };
   g_signal_connect(ok, "clicked", G_CALLBACK(+[] (GtkButton *b, gpointer win) {
                       auto f = reinterpret_cast<void (*)(GtkWidget *, gpointer)>(g_object_get_data(G_OBJECT(b), "fn"));
                       f(GTK_WIDGET(b), win); }), w);
   g_object_set_data(G_OBJECT(ok), "fn", reinterpret_cast<gpointer>(do_ok));
   g_signal_connect(e, "activate", G_CALLBACK(+[] (GtkEntry *en, gpointer win) {
                       GtkWidget *okb = GTK_WIDGET(g_object_get_data(G_OBJECT(en), "ok"));
                       g_signal_emit_by_name(okb, "clicked"); (void) win; }), w);
   g_object_set_data(G_OBJECT(e), "ok", ok);
   g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), w);
   gtk_window_present(GTK_WINDOW(w));
   gtk_widget_grab_focus(e);
}

// question with several buttons; f(i) gets the index of the button pressed (not called on cancel)
void ask_choice(const std::string &question, const std::string &detail, const std::vector<std::string> &buttons,
                int cancel_index, int default_index, std::function<void(int)> f) {
   GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", question.c_str());
   gtk_alert_dialog_set_detail(dialog, detail.c_str());
   std::vector<const char *> b;
   for (const auto &x : buttons) b.push_back(x.c_str());
   b.push_back(nullptr);
   gtk_alert_dialog_set_buttons(dialog, b.data());
   gtk_alert_dialog_set_cancel_button(dialog, cancel_index);
   gtk_alert_dialog_set_default_button(dialog, default_index);
   struct ctx_t { std::function<void(int)> f; int cancel; };
   ctx_t *ctx = new ctx_t{f, cancel_index};
   GtkWidget *w = graphics_info_t::get_main_window();
   gtk_alert_dialog_choose(dialog, w ? GTK_WINDOW(w) : nullptr, nullptr,
                           +[] (GObject *src, GAsyncResult *res, gpointer data) {
                              ctx_t *c = static_cast<ctx_t *>(data);
                              int r = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
                              if (r >= 0 && r != c->cancel) c->f(r);
                              delete c;
                           }, ctx);
   g_object_unref(dialog);
}

std::string config_path(const std::string &file) {
   return std::string(g_get_user_config_dir()) + "/coot-plus/" + file;
}

nlohmann::json read_json_file(const std::string &fn, const nlohmann::json &dflt) {
   std::ifstream f(fn);
   if (!f) return dflt;
   try { nlohmann::json j; f >> j; return j; } catch (...) { return dflt; }
}

void write_json_file(const std::string &fn, const nlohmann::json &j) {
   std::filesystem::create_directories(std::filesystem::path(fn).parent_path());
   std::ofstream f(fn);
   f << j.dump(2) << "\n";
}

// ------------------------------------------------------------------------------------
//          model and map looks (Molecules list menus) and map presets
// ------------------------------------------------------------------------------------

std::map<int, GdkRGBA> model_colours; // carbon colour chosen per model (for the swatch)

void model_draw_as(int imol, const std::string &how) {
   if (!is_valid_model_molecule(imol)) return;
   if (how == "sticks")      graphics_to_bonds_representation(imol);
   if (how == "no-waters")   graphics_to_bonds_no_waters_representation(imol);
   if (how == "ca")          graphics_to_ca_representation(imol);
   if (how == "ca-ligands")  graphics_to_ca_plus_ligands_representation(imol);
   if (how == "ribbon") {
      graphics_info_t g;
      g.add_molecular_representation(imol, "//", "Chain", "Ribbon", 2 /* CALC_SECONDARY_STRUCTURE */);
   }
   if (how == "surface")     make_molecular_surface(imol, "//");
   if (how == "no-extras") {
      for (auto &m : graphics_info_t::molecules[imol].meshes) if (!m.is_closed()) m.close();
   }
   graphics_draw();
}

void model_colour_by(int imol, const std::string &how) {
   if (!is_valid_model_molecule(imol)) return;
   if (how == "element")  { set_use_grey_carbons_for_molecule(imol, 0); model_colours.erase(imol);
                            graphics_to_bonds_representation(imol); }
   if (how == "chain")    graphics_to_colour_by_chain(imol);
   if (how == "secstr")   graphics_to_sec_struct_bonds_representation(imol);
   if (how == "bfactor")  graphics_to_b_factor_representation(imol);
   if (how == "rainbow")  graphics_to_rainbow_representation(imol);
   molecules_signature.clear();
   graphics_draw();
}

void set_model_carbon_colour(int imol, const GdkRGBA &c) {
   model_colours[imol] = c;
   set_use_grey_carbons_for_molecule(imol, 1);
   set_grey_carbon_colour(imol, c.red, c.green, c.blue);
   graphics_to_bonds_representation(imol);
   graphics_draw();
}

std::map<int, bool> map_solid; // mesh/solid chosen per map (Coot has no getter)

void map_look(int imol, const std::string &how) {
   if (!is_valid_map_molecule(imol)) return;
   if (how == "mesh")  { set_draw_solid_density_surface(imol, 0); set_draw_map_standard_lines(imol, 1); map_solid[imol] = false; }
   if (how == "solid") { set_draw_solid_density_surface(imol, 1); set_draw_map_standard_lines(imol, 0); map_solid[imol] = true; }
   if (how == "thin")   set_map_line_width(1);
   if (how == "normal") set_map_line_width(2);
   if (how == "thick")  set_map_line_width(4);
   graphics_draw();
}

nlohmann::json load_map_presets() { return read_json_file(config_path("map-presets.json"), nlohmann::json::object()); }

void save_map_preset(int imol, const std::string &name) {
   nlohmann::json j = load_map_presets();
   const GdkRGBA &c = graphics_info_t::molecules[imol].map_colour;
   j[name] = {{"r", c.red}, {"g", c.green}, {"b", c.blue},
              {"sigma", get_contour_level_in_sigma(imol)},
              {"solid", map_solid.count(imol) ? map_solid[imol] : false}};
   write_json_file(config_path("map-presets.json"), j);
   graphics_info_t::add_status_bar_text("Saved map preset “" + name + "”");
}

void apply_map_preset(int imol, const std::string &name) {
   nlohmann::json j = load_map_presets();
   if (!j.contains(name) || !is_valid_map_molecule(imol)) return;
   const auto &p = j[name];
   set_map_colour(imol, p.value("r", 0.3), p.value("g", 0.4), p.value("b", 0.8));
   set_contour_level_in_sigma(imol, p.value("sigma", 1.0));
   map_look(imol, p.value("solid", false) ? "solid" : "mesh");
   graphics_info_t::add_status_bar_text("Applied map preset “" + name + "” to " + file_basename(molecule_name(imol)));
}

void delete_map_preset(const std::string &name) {
   nlohmann::json j = load_map_presets();
   j.erase(name);
   write_json_file(config_path("map-presets.json"), j);
}

GtkWidget *make_colour_swatch(int imol, bool model) {
   GtkColorDialog *cd = gtk_color_dialog_new();
   gtk_color_dialog_set_with_alpha(cd, FALSE);
   gtk_color_dialog_set_title(cd, model ? "Carbon colour" : "Map colour");
   GtkWidget *b = gtk_color_dialog_button_new(cd);
   gtk_widget_set_focusable(b, FALSE);
   gtk_widget_set_valign(b, GTK_ALIGN_START);
   gtk_widget_add_css_class(b, "coot-plus-swatch");
   GdkRGBA c = {0.65f, 0.65f, 0.65f, 1.0f};
   if (model) {
      auto it = model_colours.find(imol);
      if (it != model_colours.end()) c = it->second;
      gtk_widget_set_tooltip_text(b, "Carbon colour of this model (other atoms keep their element colours)");
   } else {
      c = graphics_info_t::molecules[imol].map_colour;
      c.alpha = 1.0;
      gtk_widget_set_tooltip_text(b, map_is_difference_map(imol) ? "Colour of this difference map (Coot picks the negative colour)"
                                                               : "Colour of this map");
   }
   gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(b), &c);
   g_object_set_data(G_OBJECT(b), "imol", GINT_TO_POINTER(imol));
   g_object_set_data(G_OBJECT(b), "model", GINT_TO_POINTER(model ? 1 : 0));
   g_signal_connect(b, "notify::rgba", G_CALLBACK(+[] (GObject *o, GParamSpec *, gpointer) {
                       if (updating_widgets) return;
                       int imol = GPOINTER_TO_INT(g_object_get_data(o, "imol"));
                       const GdkRGBA *c = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(o));
                       if (GPOINTER_TO_INT(g_object_get_data(o, "model"))) set_model_carbon_colour(imol, *c);
                       else { set_map_colour(imol, c->red, c->green, c->blue); graphics_draw(); }
                    }), nullptr);
   // (the button owns cd: gtk_color_dialog_button_new() takes the reference)
   return b;
}

// the extra sections of a Molecules row menu
void add_model_look_entries(GtkWidget *box, GtkWidget *pop, int imol) {
   add_menu_heading(box, "Draw as");
   add_fn_entry(box, pop, "Sticks (all atoms)", nullptr, [imol] () { model_draw_as(imol, "sticks"); });
   add_fn_entry(box, pop, "Sticks without waters", nullptr, [imol] () { model_draw_as(imol, "no-waters"); });
   add_fn_entry(box, pop, "CA trace", nullptr, [imol] () { model_draw_as(imol, "ca"); });
   add_fn_entry(box, pop, "CA trace + ligands", nullptr, [imol] () { model_draw_as(imol, "ca-ligands"); });
   add_fn_entry(box, pop, "Add ribbons", "Adds a cartoon (coloured by chain) on top of the current drawing",
                [imol] () { model_draw_as(imol, "ribbon"); });
   add_fn_entry(box, pop, "Add molecular surface", nullptr, [imol] () { model_draw_as(imol, "surface"); });
   add_fn_entry(box, pop, "Remove ribbons and surfaces", nullptr, [imol] () { model_draw_as(imol, "no-extras"); });
   add_menu_heading(box, "Colour by");
   add_fn_entry(box, pop, "Element", "Standard atom colours (carbon colour from the swatch is reset)",
                [imol] () { model_colour_by(imol, "element"); });
   add_fn_entry(box, pop, "Chain", nullptr, [imol] () { model_colour_by(imol, "chain"); });
   add_fn_entry(box, pop, "Secondary structure", nullptr, [imol] () { model_colour_by(imol, "secstr"); });
   add_fn_entry(box, pop, "B-factor", nullptr, [imol] () { model_colour_by(imol, "bfactor"); });
   add_fn_entry(box, pop, "Rainbow (N → C)", nullptr, [imol] () { model_colour_by(imol, "rainbow"); });
   bool h = graphics_info_t::molecules[imol].draw_hydrogens();
   add_fn_entry(box, pop, h ? "Hide hydrogens" : "Show hydrogens", nullptr,
                [imol, h] () { set_draw_hydrogens(imol, h ? 0 : 1); molecules_signature.clear(); graphics_draw(); });
}

void add_map_look_entries(GtkWidget *box, GtkWidget *pop, int imol) {
   add_menu_heading(box, "Map look");
   add_fn_entry(box, pop, "Mesh", nullptr, [imol] () { map_look(imol, "mesh"); });
   add_fn_entry(box, pop, "Solid surface", nullptr, [imol] () { map_look(imol, "solid"); });
   add_fn_entry(box, pop, "Thin lines (all maps)", nullptr, [imol] () { map_look(imol, "thin"); });
   add_fn_entry(box, pop, "Normal lines (all maps)", nullptr, [imol] () { map_look(imol, "normal"); });
   add_fn_entry(box, pop, "Thick lines (all maps)", nullptr, [imol] () { map_look(imol, "thick"); });
   add_menu_heading(box, "Presets");
   nlohmann::json presets = load_map_presets();
   for (auto it = presets.begin(); it != presets.end(); ++it) {
      std::string name = it.key();
      add_fn_entry(box, pop, "Apply “" + name + "”", "Colour, contour (σ) and mesh/solid from the preset",
                   [imol, name] () { apply_map_preset(imol, name); });
   }
   add_fn_entry(box, pop, "Save this look as a preset…", "Saves this map's colour, contour (σ) and mesh/solid",
                [imol] () { ask_name("Save Map Preset", "Name for this map look (colour, σ, mesh/solid):",
                                     map_is_difference_map(imol) ? "Difference map" : "My 2Fo-Fc",
                                     [imol] (const std::string &n) { save_map_preset(imol, n); }); });
   for (auto it = presets.begin(); it != presets.end(); ++it) {
      std::string name = it.key();
      add_fn_entry(box, pop, "Delete preset “" + name + "”", nullptr, [name] () {
            delete_map_preset(name); graphics_info_t::add_status_bar_text("Deleted map preset “" + name + "”"); });
   }
}

// ------------------------------------------------------------------------------------
//          recent files and file groups (Load page)
// ------------------------------------------------------------------------------------


bool open_recent_file(const coot::session::recent_file_t &rf) {
   if (!std::filesystem::exists(rf.path)) return false;
   if (rf.is_coordinates())   handle_read_draw_molecule_with_recentre(rf.path, 1);
   else if (rf.is_map())      make_and_draw_map(rf.path.c_str(), rf.f_col.c_str(), rf.phi_col.c_str(),
                                                rf.weight_col.c_str(), rf.use_weights, rf.is_diff_map);
   else if (rf.is_ccp4_map()) handle_read_ccp4_map(rf.path, rf.is_diff_map);
   else if (rf.is_dictionary()) read_cif_dictionary(rf.path);
   return true;
}

std::string recent_label(const coot::session::recent_file_t &rf) {
   std::string l = file_basename(rf.path);
   if (rf.is_map()) l += "  " + rf.f_col + (rf.is_diff_map ? " (difference)" : "");
   return l;
}

std::string recent_icon(const coot::session::recent_file_t &rf) {
   if (rf.is_coordinates()) return "go-to-atom";
   if (rf.is_dictionary()) return "go-to-ligand";
   return rf.is_diff_map ? "diff-map" : "map";
}

nlohmann::json recent_to_json(const coot::session::recent_file_t &rf) {
   return {{"type", rf.type}, {"path", rf.path}, {"f_col", rf.f_col}, {"phi_col", rf.phi_col},
           {"weight_col", rf.weight_col}, {"use_weights", rf.use_weights}, {"is_diff_map", rf.is_diff_map}};
}

coot::session::recent_file_t json_to_recent(const nlohmann::json &j) {
   coot::session::recent_file_t rf;
   rf.type = j.value("type", ""); rf.path = j.value("path", "");
   rf.f_col = j.value("f_col", ""); rf.phi_col = j.value("phi_col", ""); rf.weight_col = j.value("weight_col", "");
   rf.use_weights = j.value("use_weights", false); rf.is_diff_map = j.value("is_diff_map", false);
   return rf;
}

// what is open now, as files that can be opened again
std::vector<coot::session::recent_file_t> capture_open_files() {
   std::vector<coot::session::recent_file_t> v;
   for (int imol=0; imol<graphics_n_molecules(); imol++) {
      coot::session::recent_file_t rf;
      molecule_class_info_t &m = graphics_info_t::molecules[imol];
      if (is_valid_model_molecule(imol)) {
         std::error_code ec;
         rf.type = "coordinates";
         rf.path = std::filesystem::absolute(molecule_name(imol), ec).lexically_normal().string();
         if (!std::filesystem::exists(rf.path)) continue; // e.g. a molecule made in Coot, never saved
      } else if (is_valid_map_molecule(imol)) {
         rf.is_diff_map = map_is_difference_map(imol);
         if (!m.save_mtz_file_name.empty()) {
            std::error_code ec;
            rf.type = "map";
            rf.path = std::filesystem::absolute(m.save_mtz_file_name, ec).lexically_normal().string();
            rf.f_col = m.save_f_col; rf.phi_col = m.save_phi_col; rf.weight_col = m.save_weight_col;
            rf.use_weights = m.save_use_weights;
         } else {
            std::error_code ec;
            rf.type = "ccp4-map";
            rf.path = std::filesystem::absolute(molecule_name(imol), ec).lexically_normal().string();
         }
         if (!std::filesystem::exists(rf.path)) continue;
      } else {
         continue;
      }
      v.push_back(rf);
   }
   if (graphics_info_t::cif_dictionary_filename_vec)
      for (const auto &d : *graphics_info_t::cif_dictionary_filename_vec) {
         coot::session::recent_file_t rf;
         rf.type = "dictionary";
         std::error_code ec;
         rf.path = std::filesystem::absolute(d, ec).lexically_normal().string();
         if (std::filesystem::exists(rf.path)) v.push_back(rf);
      }
   return v;
}

nlohmann::json load_groups() { return read_json_file(config_path("groups.json"), nlohmann::json::array()); }
void save_groups(const nlohmann::json &j) { write_json_file(config_path("groups.json"), j); }

void save_open_files_as_group(const std::string &name) {
   std::vector<coot::session::recent_file_t> files = capture_open_files();
   if (files.empty()) { graphics_info_t::add_status_bar_text("Nothing open that can be saved as a group"); return; }
   nlohmann::json groups = load_groups();
   nlohmann::json items = nlohmann::json::array();
   for (const auto &f : files) items.push_back(recent_to_json(f));
   bool replaced = false;
   for (auto &g : groups) if (g.value("name", "") == name) { g["items"] = items; replaced = true; }
   if (!replaced) groups.push_back({{"name", name}, {"items", items}});
   save_groups(groups);
   graphics_info_t::add_status_bar_text("Saved group “" + name + "” (" + std::to_string(files.size()) + " files)");
   refresh_recent_and_groups();
}

void open_group_files(const nlohmann::json &group, bool replace) {
   if (replace)
      for (int imol=0; imol<graphics_n_molecules(); imol++)
         if (is_valid_model_molecule(imol) || is_valid_map_molecule(imol)) close_molecule(imol);
   // dictionaries first, so the models' ligands get their restraints
   std::vector<coot::session::recent_file_t> files;
   for (const auto &j : group.value("items", nlohmann::json::array())) files.push_back(json_to_recent(j));
   std::stable_sort(files.begin(), files.end(), [] (const coot::session::recent_file_t &a, const coot::session::recent_file_t &b) {
                       auto rank = [] (const coot::session::recent_file_t &f) { return f.is_dictionary() ? 0 : (f.is_coordinates() ? 1 : 2); };
                       return rank(a) < rank(b); });
   std::vector<std::string> missing;
   int n = 0;
   for (const auto &f : files) { if (open_recent_file(f)) n++; else missing.push_back(file_basename(f.path)); }
   std::string name = group.value("name", "group");
   graphics_info_t::add_status_bar_text("Opened group “" + name + "”: " + std::to_string(n) + " files");
   if (!missing.empty()) {
      std::string m;
      for (const auto &x : missing) m += (m.empty() ? "" : ", ") + x;
      coot_plus_message("warning", "Group “" + name + "”: file(s) not found: " + m,
                        "They were moved or deleted - update the group with what's open now");
   }
   graphics_draw();
}

void ask_open_group(const nlohmann::json &group) {
   bool any = false;
   for (int imol=0; imol<graphics_n_molecules(); imol++)
      if (is_valid_model_molecule(imol) || is_valid_map_molecule(imol)) any = true;
   if (!any) { open_group_files(group, false); return; }
   nlohmann::json g = group;
   ask_choice("Open group “" + group.value("name", "") + "”", "Models and maps are already open. "
              "Close them first (recommended - maps and models from different data sets won't match), or add the group to them?",
              {"Cancel", "Add to Open Files", "Close Others and Open"}, 0, 2,
              [g] (int r) { open_group_files(g, r == 2); });
}

GtkWidget *make_file_row(const std::string &icon, const std::string &title, const std::string &subtitle,
                         const std::string &tooltip) {
   GtkWidget *b = gtk_button_new();
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
   GtkWidget *img = icon.empty() ? gtk_image_new() : gtk_image_new_from_icon_name(modern_icon(icon).c_str());
   gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
   gtk_widget_set_valign(img, GTK_ALIGN_START);
   GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
   GtkWidget *t = gtk_label_new(title.c_str());
   GtkWidget *st = gtk_label_new(subtitle.c_str());
   for (GtkWidget *l : {t, st}) {
      gtk_label_set_xalign(GTK_LABEL(l), 0.0);
      gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
      gtk_label_set_max_width_chars(GTK_LABEL(l), 22);
      gtk_box_append(GTK_BOX(texts), l);
   }
   gtk_widget_add_css_class(st, "coot-plus-dim");
   gtk_box_append(GTK_BOX(box), img);
   gtk_box_append(GTK_BOX(box), texts);
   gtk_button_set_child(GTK_BUTTON(b), box);
   gtk_widget_add_css_class(b, "flat");
   gtk_widget_add_css_class(b, "coot-plus-tool");
   gtk_widget_set_focusable(b, FALSE);
   gtk_widget_set_tooltip_text(b, tooltip.c_str());
   gtk_widget_set_hexpand(b, TRUE);
   return b;
}

void refresh_recent_and_groups() {
   if (recent_box) {
      GtkWidget *c;
      while ((c = gtk_widget_get_first_child(recent_box)) != nullptr) gtk_box_remove(GTK_BOX(recent_box), c);
      std::vector<coot::session::recent_file_t> rfs = coot::session::get_recent_files();
      unsigned int shown = 0;
      for (const auto &rf : rfs) {
         if (shown >= 8) break;
         if (rf.path.find("/coot-workbench/build/") != std::string::npos) continue; // Coot+ test data
         std::string folder = file_basename(std::filesystem::path(rf.path).parent_path().string());
         std::string tip = rf.path;
         if (rf.is_map()) tip += "\ncolumns " + rf.f_col + " " + rf.phi_col + (rf.is_diff_map ? " (difference map)" : "");
         if (!std::filesystem::exists(rf.path)) tip += "\n(file not found)";
         GtkWidget *b = make_file_row(recent_icon(rf), recent_label(rf), folder, tip);
         auto *copy = new coot::session::recent_file_t(rf);
         g_signal_connect_data(b, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer d) {
                                  auto rf = *static_cast<coot::session::recent_file_t *>(d);
                                  if (!open_recent_file(rf))
                                     coot_plus_message("warning", "File not found: " + rf.path, "It was moved or deleted");
                                  g_idle_add(+[] (gpointer) -> gboolean { refresh_recent_and_groups(); return G_SOURCE_REMOVE; }, nullptr);
                               }), copy, +[] (gpointer d, GClosure *) { delete static_cast<coot::session::recent_file_t *>(d); },
                               GConnectFlags(0));
         gtk_box_append(GTK_BOX(recent_box), b);
         shown++;
      }
      if (shown == 0) {
         GtkWidget *l = gtk_label_new("Files you open will be listed here.");
         gtk_label_set_xalign(GTK_LABEL(l), 0.0);
         gtk_widget_add_css_class(l, "coot-plus-page-hint");
         gtk_box_append(GTK_BOX(recent_box), l);
      }
   }
   if (groups_box) {
      GtkWidget *c;
      while ((c = gtk_widget_get_first_child(groups_box)) != nullptr) gtk_box_remove(GTK_BOX(groups_box), c);
      nlohmann::json groups = load_groups();
      for (const auto &g : groups) {
         std::string name = g.value("name", "group");
         int n_models = 0, n_maps = 0, n_dicts = 0;
         std::string tip = name + ":";
         for (const auto &it : g.value("items", nlohmann::json::array())) {
            auto rf = json_to_recent(it);
            if (rf.is_coordinates()) n_models++; else if (rf.is_dictionary()) n_dicts++; else n_maps++;
            tip += "\n  " + recent_label(rf) + (std::filesystem::exists(rf.path) ? "" : "  (not found)");
         }
         std::string sub = std::to_string(n_models) + (n_models == 1 ? " model" : " models") + ", " +
                           std::to_string(n_maps) + (n_maps == 1 ? " map" : " maps");
         if (n_dicts) sub += ", " + std::to_string(n_dicts) + " dict.";
         GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
         GtkWidget *b = make_file_row("coot-plus-load-symbolic", name, sub, tip + "\n\nClick to open");
         auto *gcopy = new nlohmann::json(g);
         g_signal_connect_data(b, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer d) {
                                  ask_open_group(*static_cast<nlohmann::json *>(d)); }),
                               gcopy, +[] (gpointer d, GClosure *) { delete static_cast<nlohmann::json *>(d); }, GConnectFlags(0));
         GtkWidget *mb = gtk_menu_button_new();
         gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(mb), "coot-plus-more-symbolic");
         gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(mb), FALSE);
         gtk_widget_set_focusable(mb, FALSE);
         gtk_widget_set_valign(mb, GTK_ALIGN_CENTER);
         GtkWidget *pop = gtk_popover_new();
         GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
         add_fn_entry(pbox, pop, "Open (add to what's open)", nullptr, [g] () { open_group_files(g, false); });
         add_fn_entry(pbox, pop, "Close others and open", nullptr, [g] () { open_group_files(g, true); });
         add_fn_entry(pbox, pop, "Update to what's open now", "Replace this group's files with the models and maps open now",
                      [name] () { save_open_files_as_group(name); });
         add_fn_entry(pbox, pop, "Rename…", nullptr, [name] () {
               ask_name("Rename Group", "New name:", name, [name] (const std::string &n) {
                     nlohmann::json gs = load_groups();
                     for (auto &x : gs) if (x.value("name", "") == name) x["name"] = n;
                     save_groups(gs); refresh_recent_and_groups(); }); });
         add_fn_entry(pbox, pop, "Delete…", nullptr, [name] () {
               ask_choice("Delete group “" + name + "”?", "Only the group is removed - the files stay on disk.",
                          {"Cancel", "Delete"}, 0, 1, [name] (int) {
                             nlohmann::json gs = load_groups(), out = nlohmann::json::array();
                             for (auto &x : gs) if (x.value("name", "") != name) out.push_back(x);
                             save_groups(out); refresh_recent_and_groups(); }); });
         gtk_popover_set_child(GTK_POPOVER(pop), pbox);
         gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), pop);
         gtk_box_append(GTK_BOX(row), b);
         gtk_box_append(GTK_BOX(row), mb);
         gtk_box_append(GTK_BOX(groups_box), row);
      }
      GtkWidget *save = gtk_button_new_with_label("Save open files as a group…");
      gtk_widget_add_css_class(save, "flat");
      gtk_widget_add_css_class(save, "coot-plus-link");
      gtk_widget_set_focusable(save, FALSE);
      gtk_widget_set_tooltip_text(save, "Remember the models, maps (with their MTZ columns) and dictionaries that are open now, "
                                        "so they can be opened together with one click");
      g_signal_connect(save, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) {
                          std::vector<coot::session::recent_file_t> f = capture_open_files();
                          std::string suggestion = "My project";
                          for (const auto &x : f) if (x.is_coordinates()) {
                             suggestion = file_basename(std::filesystem::path(x.path).parent_path().string()) + " – " +
                                          std::filesystem::path(x.path).stem().string();
                             break; }
                          ask_name("Save Group", "Name for this group of files:", suggestion,
                                   [] (const std::string &n) { save_open_files_as_group(n); }); }), nullptr);
      gtk_box_append(GTK_BOX(groups_box), save);
   }
}

// ------------------------------------------------------------------------------------
//          go to residue from the search box ("A 212", "A212", "212")
// ------------------------------------------------------------------------------------

std::vector<command_t> residue_search_results(const std::string &query_in) {
   std::vector<command_t> out;
   std::string q = query_in;
   for (auto &ch : q) if (ch == '/' || ch == ':' || ch == ',') ch = ' ';
   std::string chain;
   int resno = 0;
   std::string ins;
   // [chain] number [insertion code]
   size_t i = 0;
   while (i < q.size() && q[i] == ' ') i++;
   size_t cs = i;
   while (i < q.size() && std::isalpha(static_cast<unsigned char>(q[i]))) i++;
   chain = q.substr(cs, i - cs);
   if (chain.size() > 4) return out;
   while (i < q.size() && q[i] == ' ') i++;
   size_t ns = i;
   if (i < q.size() && q[i] == '-') i++;
   while (i < q.size() && std::isdigit(static_cast<unsigned char>(q[i]))) i++;
   if (i == ns || (i == ns + 1 && q[ns] == '-')) return out;
   resno = std::atoi(q.substr(ns, i - ns).c_str());
   if (i < q.size() && std::isalpha(static_cast<unsigned char>(q[i]))) { ins = q.substr(i, 1); i++; }
   while (i < q.size() && q[i] == ' ') i++;
   if (i != q.size()) return out;

   for (int imol=0; imol<graphics_n_molecules() && out.size() < 6; imol++) {
      if (!is_valid_model_molecule(imol) || !mol_is_displayed(imol)) continue;
      mmdb::Manager *mol = graphics_info_t::molecules[imol].atom_sel.mol;
      if (!mol) continue;
      mmdb::Model *model = mol->GetModel(1);
      if (!model) continue;
      for (int ic=0; ic<model->GetNumberOfChains() && out.size() < 6; ic++) {
         mmdb::Chain *c = model->GetChain(ic);
         std::string cid = c->GetChainID();
         if (!chain.empty() && cid != chain && to_lower(cid) != to_lower(chain)) continue;
         mmdb::Residue *r = c->GetResidue(resno, ins.c_str());
         if (!r || r->GetNumberOfAtoms() == 0) continue;
         std::string rn = r->GetResName();
         // the atom to centre on: CA for amino acids, C1' for nucleotides, C1 for sugars, O for water
         std::string atom_name;
         for (const char *pref : {" CA ", " C1'", " C1 ", " O  ", " P  "}) {
            for (int ia=0; ia<r->GetNumberOfAtoms(); ia++)
               if (std::string(r->GetAtom(ia)->name) == pref) { atom_name = pref; break; }
            if (!atom_name.empty()) break;
         }
         if (atom_name.empty()) atom_name = r->GetAtom(0)->name;
         command_t cmd;
         cmd.label = "Go to " + cid + " " + std::to_string(resno) + ins + "  " + residue_display_name(rn);
         cmd.path = file_basename(molecule_name(imol)) + " (#" + std::to_string(imol) + ")";
         cmd.description = "Centres the view on this residue";
         std::string an = atom_name;
         cmd.run = [imol, cid, resno, ins, an] () {
            set_go_to_atom_molecule(imol);
            set_go_to_atom_chain_residue_atom_name_full(cid.c_str(), resno, ins.c_str(), an.c_str(), "");
         };
         out.push_back(cmd);
      }
   }
   return out;
}

// ------------------------------------------------------------------------------------
//          Python console window
// ------------------------------------------------------------------------------------

GtkWidget *console_window = nullptr;
GtkTextBuffer *console_out = nullptr;
GtkWidget *console_out_view = nullptr;
GtkWidget *console_in = nullptr;
GtkWidget *console_suggest = nullptr;   // list box
GtkWidget *console_suggest_scroll = nullptr;
GtkWidget *console_doc = nullptr;
std::vector<std::string> console_completions;
int console_history_pos = -1;
std::string console_draft;

std::string console_input_text() {
   GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(console_in));
   GtkTextIter s, e;
   gtk_text_buffer_get_bounds(b, &s, &e);
   char *t = gtk_text_buffer_get_text(b, &s, &e, FALSE);
   std::string r = t; g_free(t);
   return r;
}

void console_set_input(const std::string &t) {
   GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(console_in));
   gtk_text_buffer_set_text(b, t.c_str(), -1);
   GtkTextIter e; gtk_text_buffer_get_end_iter(b, &e); gtk_text_buffer_place_cursor(b, &e);
}

void console_print(const std::string &text, const char *tag) {
   GtkTextIter end;
   gtk_text_buffer_get_end_iter(console_out, &end);
   gtk_text_buffer_insert_with_tags_by_name(console_out, &end, text.c_str(), -1, tag, nullptr);
   gtk_text_buffer_get_end_iter(console_out, &end);
   GtkTextMark *m = gtk_text_buffer_create_mark(console_out, nullptr, &end, FALSE);
   gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(console_out_view), m);
   gtk_text_buffer_delete_mark(console_out, m);
}

// the identifier being typed before the cursor, e.g. "coot.set_zo"
std::string console_current_token() {
   GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(console_in));
   GtkTextIter cur, start;
   gtk_text_buffer_get_iter_at_mark(b, &cur, gtk_text_buffer_get_insert(b));
   start = cur;
   while (gtk_text_iter_backward_char(&start)) {
      gunichar c = gtk_text_iter_get_char(&start);
      if (!(g_unichar_isalnum(c) || c == '_' || c == '.')) { gtk_text_iter_forward_char(&start); break; }
   }
   char *t = gtk_text_buffer_get_text(b, &start, &cur, FALSE);
   std::string r = t; g_free(t);
   return r;
}

// names in coot / coot_utils / coot_gui / __main__ that complete the token
std::vector<std::string> python_completions(const std::string &token) {
   std::vector<std::string> out;
   if (token.size() < 2) return out;
   PyGILState_STATE gil = PyGILState_Ensure();
   PyObject *modules = PySys_GetObject("modules"); // borrowed
   std::string mod_name, prefix = token;
   size_t dot = token.rfind('.');
   if (dot != std::string::npos) { mod_name = token.substr(0, dot); prefix = token.substr(dot + 1); }
   auto add_from = [&] (PyObject *obj, const std::string &qual) {
      PyObject *names = obj ? PyObject_Dir(obj) : nullptr;
      if (!names) { PyErr_Clear(); return; }
      Py_ssize_t n = PyList_Size(names);
      for (Py_ssize_t i=0; i<n && out.size() < 200; i++) {
         const char *c = PyUnicode_AsUTF8(PyList_GetItem(names, i));
         if (!c || c[0] == '_') continue;
         std::string nm(c);
         if (nm.rfind(prefix, 0) == 0) out.push_back(qual.empty() ? nm : qual + "." + nm);
      }
      Py_DECREF(names);
   };
   if (!mod_name.empty()) {
      add_from(PyDict_GetItemString(modules, mod_name.c_str()), mod_name);
   } else {
      PyObject *main_dict = PyModule_GetDict(PyImport_AddModule("__main__"));
      PyObject *keys = PyDict_Keys(main_dict);
      for (Py_ssize_t i=0; keys && i<PyList_Size(keys); i++) {
         const char *c = PyUnicode_AsUTF8(PyList_GetItem(keys, i));
         if (c && c[0] != '_' && std::string(c).rfind(prefix, 0) == 0) out.push_back(c);
      }
      Py_XDECREF(keys);
      // a bare function name: offer the coot module's (the commonest NameError)
      add_from(PyDict_GetItemString(modules, "coot"), "coot");
      add_from(PyDict_GetItemString(modules, "coot_utils"), "coot_utils");
   }
   PyErr_Clear();
   PyGILState_Release(gil);
   std::sort(out.begin(), out.end(), [] (const std::string &a, const std::string &b) {
                return a.size() != b.size() ? a.size() < b.size() : a < b; });
   out.erase(std::unique(out.begin(), out.end()), out.end());
   if (out.size() > 40) out.resize(40);
   return out;
}

// first lines of a function's docstring: signature + summary
std::string python_doc(const std::string &qualified) {
   std::string r;
   size_t dot = qualified.rfind('.');
   if (dot == std::string::npos) return r;
   PyGILState_STATE gil = PyGILState_Ensure();
   PyObject *mod = PyDict_GetItemString(PySys_GetObject("modules"), qualified.substr(0, dot).c_str());
   PyObject *f = mod ? PyObject_GetAttrString(mod, qualified.substr(dot + 1).c_str()) : nullptr;
   PyObject *doc = f ? PyObject_GetAttrString(f, "__doc__") : nullptr;
   if (doc && PyUnicode_Check(doc)) {
      std::string d = PyUnicode_AsUTF8(doc);
      std::vector<std::string> lines;
      std::string line;
      for (char ch : d) { if (ch == '\n') { if (!line.empty()) lines.push_back(line); line.clear(); } else line += ch; }
      if (!line.empty()) lines.push_back(line);
      for (unsigned int i=0; i<lines.size() && i<3; i++) {
         if (lines[i].find("Parameters") != std::string::npos || lines[i].find("----") != std::string::npos) break;
         r += (r.empty() ? "" : "\n") + lines[i];
      }
   }
   Py_XDECREF(doc); Py_XDECREF(f);
   PyErr_Clear();
   PyGILState_Release(gil);
   return r;
}

void console_hide_suggestions() {
   if (console_suggest_scroll) gtk_widget_set_visible(console_suggest_scroll, FALSE);
   console_completions.clear();
}

void console_show_doc_for_selected() {
   GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(console_suggest));
   if (!row) return;
   int i = gtk_list_box_row_get_index(row);
   if (i < 0 || i >= int(console_completions.size())) return;
   std::string d = python_doc(console_completions[i]);
   gtk_label_set_text(GTK_LABEL(console_doc), d.empty() ? console_completions[i].c_str() : d.c_str());
}

void console_update_suggestions() {
   std::string tok = console_current_token();
   console_completions = python_completions(tok);
   GtkWidget *c;
   while ((c = gtk_widget_get_first_child(console_suggest)) != nullptr) gtk_list_box_remove(GTK_LIST_BOX(console_suggest), c);
   if (console_completions.empty() || (console_completions.size() == 1 && console_completions[0] == tok)) {
      console_hide_suggestions();
      return;
   }
   for (const auto &n : console_completions) {
      GtkWidget *l = gtk_label_new(n.c_str());
      gtk_label_set_xalign(GTK_LABEL(l), 0.0);
      gtk_widget_add_css_class(l, "coot-plus-mono");
      gtk_list_box_append(GTK_LIST_BOX(console_suggest), l);
   }
   gtk_list_box_select_row(GTK_LIST_BOX(console_suggest), gtk_list_box_get_row_at_index(GTK_LIST_BOX(console_suggest), 0));
   gtk_widget_set_visible(console_suggest_scroll, TRUE);
   console_show_doc_for_selected();
}

void console_accept_suggestion() {
   GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(console_suggest));
   if (!row) return;
   int i = gtk_list_box_row_get_index(row);
   if (i < 0 || i >= int(console_completions.size())) return;
   std::string full = console_completions[i];
   std::string tok = console_current_token();
   GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(console_in));
   GtkTextIter cur, start;
   gtk_text_buffer_get_iter_at_mark(b, &cur, gtk_text_buffer_get_insert(b));
   start = cur;
   gtk_text_iter_backward_chars(&start, g_utf8_strlen(tok.c_str(), -1));
   gtk_text_buffer_delete(b, &start, &cur);
   std::string ins = full + "(";
   gtk_text_buffer_insert_at_cursor(b, ins.c_str(), -1);
   std::string d = python_doc(full);
   if (!d.empty()) gtk_label_set_text(GTK_LABEL(console_doc), d.c_str());
   console_hide_suggestions();
}

void console_run() {
   std::string code = console_input_text();
   while (!code.empty() && (code.back() == '\n' || code.back() == ' ')) code.pop_back();
   if (code.empty()) return;
   console_hide_suggestions();
   std::string shown;
   bool first = true;
   std::string line;
   for (char ch : code + "\n") {
      if (ch == '\n') { shown += (first ? ">>> " : "... ") + line + "\n"; first = false; line.clear(); }
      else line += ch;
   }
   console_print(shown, "in");
   std::string out, err;
   bool ok = coot_plus_run_python(code, out, err);
   if (!out.empty()) console_print(out + (out.back() == '\n' ? "" : "\n"), "out");
   if (!ok) {
      console_print(err + "\n", "err");
      if (err.find("NameError") != std::string::npos && err.find("did you mean") == std::string::npos)
         console_print("Hint: Coot functions are in the coot module - type coot. and press Tab\n", "hint");
   }
   graphics_info_t::command_history.add_to_history(code);
   console_history_pos = -1;
   console_set_input("");
   graphics_draw();
}

void console_history_step(int dir) {
   const auto &h = graphics_info_t::command_history.commands;
   if (h.empty()) return;
   if (console_history_pos == -1) { console_draft = console_input_text(); console_history_pos = h.size(); }
   int p = console_history_pos + dir;
   if (p < 0) p = 0;
   if (p >= int(h.size())) { console_history_pos = -1; console_set_input(console_draft); return; }
   console_history_pos = p;
   console_set_input(h[p]);
}

void show_python_console() {
   if (!console_window) {
      GtkWidget *w = gtk_window_new();
      gtk_window_set_title(GTK_WINDOW(w), "Coot+ Python Console");
      gtk_window_set_default_size(GTK_WINDOW(w), 780, 520);
      gtk_window_set_hide_on_close(GTK_WINDOW(w), TRUE);
      if (GtkWidget *mw = graphics_info_t::get_main_window()) gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(mw));
      GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
      gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8);
      gtk_widget_set_margin_top(box, 8);   gtk_widget_set_margin_bottom(box, 8);

      console_out_view = gtk_text_view_new();
      gtk_text_view_set_monospace(GTK_TEXT_VIEW(console_out_view), TRUE);
      gtk_text_view_set_editable(GTK_TEXT_VIEW(console_out_view), FALSE);
      gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(console_out_view), GTK_WRAP_WORD_CHAR);
      gtk_text_view_set_left_margin(GTK_TEXT_VIEW(console_out_view), 8);
      gtk_text_view_set_top_margin(GTK_TEXT_VIEW(console_out_view), 6);
      gtk_widget_add_css_class(console_out_view, "coot-plus-console-out");
      console_out = gtk_text_view_get_buffer(GTK_TEXT_VIEW(console_out_view));
      gtk_text_buffer_create_tag(console_out, "in",   "foreground", "#8ab4f8", nullptr);
      gtk_text_buffer_create_tag(console_out, "out",  nullptr);
      gtk_text_buffer_create_tag(console_out, "err",  "foreground", "#ff7b72", nullptr);
      gtk_text_buffer_create_tag(console_out, "hint", "foreground", "#e3b341", nullptr);
      gtk_text_buffer_create_tag(console_out, "info", "foreground", "#8b949e", nullptr);
      console_print("Coot+ Python console. The coot, coot_utils and coot_gui modules are loaded.\n"
                    "Return runs · Shift-Return new line · Tab completes (type coot. and Tab) · ↑/↓ history\n"
                    "Example: coot.set_zoom(40)    coot.molecule_name(0)\n\n", "info");
      GtkWidget *sw = gtk_scrolled_window_new();
      gtk_widget_set_vexpand(sw, TRUE);
      gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), console_out_view);

      console_suggest = gtk_list_box_new();
      gtk_list_box_set_selection_mode(GTK_LIST_BOX(console_suggest), GTK_SELECTION_BROWSE);
      gtk_widget_set_focusable(console_suggest, FALSE);
      g_signal_connect(console_suggest, "row-selected", G_CALLBACK(+[] (GtkListBox *, GtkListBoxRow *, gpointer) {
                          console_show_doc_for_selected(); }), nullptr);
      g_signal_connect(console_suggest, "row-activated", G_CALLBACK(+[] (GtkListBox *, GtkListBoxRow *, gpointer) {
                          console_accept_suggestion(); gtk_widget_grab_focus(console_in); }), nullptr);
      console_suggest_scroll = gtk_scrolled_window_new();
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(console_suggest_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
      gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(console_suggest_scroll), 120);
      gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(console_suggest_scroll), console_suggest);
      gtk_widget_set_visible(console_suggest_scroll, FALSE);

      console_doc = gtk_label_new("Help for the function you're typing appears here.");
      gtk_label_set_xalign(GTK_LABEL(console_doc), 0.0);
      gtk_label_set_wrap(GTK_LABEL(console_doc), TRUE);
      gtk_label_set_selectable(GTK_LABEL(console_doc), TRUE);
      gtk_widget_add_css_class(console_doc, "coot-plus-console-doc");

      console_in = gtk_text_view_new();
      gtk_text_view_set_monospace(GTK_TEXT_VIEW(console_in), TRUE);
      gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(console_in), GTK_WRAP_WORD_CHAR);
      gtk_text_view_set_left_margin(GTK_TEXT_VIEW(console_in), 8);
      gtk_text_view_set_top_margin(GTK_TEXT_VIEW(console_in), 6);
      gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(console_in), 6);
      gtk_widget_add_css_class(console_in, "coot-plus-console-in");
      GtkWidget *in_sw = gtk_scrolled_window_new();
      gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(in_sw), 60);
      gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(in_sw), 160);
      gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(in_sw), TRUE);
      gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(in_sw), console_in);

      GtkEventController *keys = gtk_event_controller_key_new();
      gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
      g_signal_connect(keys, "key-pressed", G_CALLBACK(+[] (GtkEventControllerKey *, guint keyval, guint, GdkModifierType state,
                                                             gpointer) -> gboolean {
                          bool suggestions = console_suggest_scroll && gtk_widget_get_visible(console_suggest_scroll);
                          if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && !(state & GDK_SHIFT_MASK)) {
                             if (suggestions) { console_accept_suggestion(); return TRUE; }
                             console_run(); return TRUE;
                          }
                          if (keyval == GDK_KEY_Tab) {
                             if (suggestions) console_accept_suggestion(); else console_update_suggestions();
                             return TRUE;
                          }
                          if (keyval == GDK_KEY_Escape && suggestions) { console_hide_suggestions(); return TRUE; }
                          if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) {
                             int d = keyval == GDK_KEY_Up ? -1 : 1;
                             if (suggestions) {
                                GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(console_suggest));
                                int i = row ? gtk_list_box_row_get_index(row) + d : 0;
                                i = std::max(0, std::min(int(console_completions.size()) - 1, i));
                                GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(console_suggest), i);
                                gtk_list_box_select_row(GTK_LIST_BOX(console_suggest), r);
                                gtk_widget_grab_focus(console_in);
                                return TRUE;
                             }
                             // history only when the input is one line
                             if (console_input_text().find('\n') == std::string::npos) { console_history_step(d); return TRUE; }
                          }
                          return FALSE;
                       }), nullptr);
      gtk_widget_add_controller(console_in, keys);
      // live suggestions while typing an identifier
      g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(console_in)), "changed",
                       G_CALLBACK(+[] (GtkTextBuffer *, gpointer) {
                          std::string t = console_current_token();
                          if (t.size() >= 3 || (t.size() >= 2 && t.back() == '.') || t.find('.') != std::string::npos)
                             console_update_suggestions();
                          else console_hide_suggestions();
                       }), nullptr);

      GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      GtkWidget *run = gtk_button_new_with_label("Run  (Return)");
      gtk_widget_add_css_class(run, "suggested-action");
      GtkWidget *clear = gtk_button_new_with_label("Clear Output");
      GtkWidget *log = gtk_button_new_with_label("Command Log…");
      g_signal_connect(run, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { console_run(); gtk_widget_grab_focus(console_in); }), nullptr);
      g_signal_connect(clear, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { gtk_text_buffer_set_text(console_out, "", -1); }), nullptr);
      g_signal_connect(log, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { show_command_log(); }), nullptr);
      GtkWidget *spacer = gtk_label_new("");
      gtk_widget_set_hexpand(spacer, TRUE);
      gtk_box_append(GTK_BOX(buttons), log);
      gtk_box_append(GTK_BOX(buttons), spacer);
      gtk_box_append(GTK_BOX(buttons), clear);
      gtk_box_append(GTK_BOX(buttons), run);

      gtk_box_append(GTK_BOX(box), sw);
      gtk_box_append(GTK_BOX(box), console_suggest_scroll);
      gtk_box_append(GTK_BOX(box), console_doc);
      gtk_box_append(GTK_BOX(box), in_sw);
      gtk_box_append(GTK_BOX(box), buttons);
      console_window = w;
      console_notebook = gtk_notebook_new();
      gtk_notebook_append_page(GTK_NOTEBOOK(console_notebook), box, gtk_label_new("Console"));
      gtk_notebook_append_page(GTK_NOTEBOOK(console_notebook), make_script_page(), gtk_label_new("Script"));
      gtk_notebook_append_page(GTK_NOTEBOOK(console_notebook), make_snippets_page(), gtk_label_new("Snippets"));
      gtk_window_set_child(GTK_WINDOW(w), console_notebook);
   }
   gtk_window_present(GTK_WINDOW(console_window));
   gtk_widget_grab_focus(console_in);
}

// ------------------------------------------------------------------------------------
//          compact sidebar
// ------------------------------------------------------------------------------------


void apply_compact_sidebar(bool on) {
   compact_sidebar = on;
   for (GtkWidget *w : sidebar_shortcut_labels) gtk_widget_set_visible(w, !on);
   for (GtkWidget *w : sidebar_hint_labels)     gtk_widget_set_visible(w, !on);
   for (GtkWidget *w : sidebar_tool_labels)     gtk_label_set_max_width_chars(GTK_LABEL(w), on ? 14 : 19);
   if (sidebar_help_box) gtk_widget_set_visible(sidebar_help_box, !on);
   if (sidebar) gtk_widget_set_size_request(sidebar, on ? 150 : 210, -1);
}

void set_compact_sidebar(bool on) {
   apply_compact_sidebar(on);
   save_coot_plus_setting_bool("compact-sidebar", on);
   graphics_info_t::add_status_bar_text(on ? "Compact sidebar" : "Full sidebar");
}


// ------------------------------------------------------------------------------------
//          calling the Coot+ Python helpers (python/coot_plus.py)
// ------------------------------------------------------------------------------------

// evaluate a Python expression (with coot, coot_utils and coot_plus imported) and
// return its value as JSON (null on error - the error goes to the terminal/log)
nlohmann::json py_json(const std::string &expr) {
   ignore_history_scope ignore; // the coot calls inside are not the user's commands
   nlohmann::json j;
   PyGILState_STATE gil = PyGILState_Ensure();
   PyObject *main_dict = PyModule_GetDict(PyImport_AddModule("__main__"));
   PyRun_SimpleString("import json as __cp_json, coot_plus as __cp_plus, coot as __cp_coot");
   std::string code = "__cp_json.dumps(" + expr + ")";
   PyObject *r = PyRun_String(code.c_str(), Py_eval_input, main_dict, main_dict);
   if (r && PyUnicode_Check(r)) {
      j = nlohmann::json::parse(PyUnicode_AsUTF8(r), nullptr, false);
      if (j.is_discarded()) j = nullptr;
   } else {
      PyErr_Print();
   }
   Py_XDECREF(r);
   PyErr_Clear();
   PyGILState_Release(gil);
   return j;
}

std::string py_str(const std::string &s) { // a Python string literal
   nlohmann::json j = s;
   return j.dump();
}

// the map to use: the refinement map, else the first non-difference map, else any map
int best_map() {
   int r = imol_refinement_map();
   if (is_valid_map_molecule(r) && !map_is_difference_map(r)) return r;
   for (int i=0; i<graphics_n_molecules(); i++) if (is_valid_map_molecule(i) && !map_is_difference_map(i)) return i;
   for (int i=0; i<graphics_n_molecules(); i++) if (is_valid_map_molecule(i)) return i;
   return -1;
}

// the model to use: the one at the centre, else the biggest
int best_model() {
   std::pair<bool, std::pair<int, coot::atom_spec_t> > aa = graphics_info_t::active_atom_spec();
   if (aa.first && is_valid_model_molecule(aa.second.first)) return aa.second.first;
   int best = -1, n = 0;
   for (int i=0; i<graphics_n_molecules(); i++)
      if (is_valid_model_molecule(i) && graphics_info_t::molecules[i].atom_sel.n_selected_atoms > n) {
         best = i; n = graphics_info_t::molecules[i].atom_sel.n_selected_atoms; }
   return best;
}

// go to a residue, centring on its CA (or C1', C1, O, P, or the first atom)
void go_to_residue(int imol, const std::string &chain, int resno, const std::string &ins) {
   if (!is_valid_model_molecule(imol)) return;
   mmdb::Residue *r = graphics_info_t::molecules[imol].get_residue(chain, resno, ins);
   if (!r || r->GetNumberOfAtoms() == 0) return;
   std::string atom_name;
   for (const char *pref : {" CA ", " C1'", " C1 ", " O  ", " P  "}) {
      for (int ia=0; ia<r->GetNumberOfAtoms(); ia++)
         if (std::string(r->GetAtom(ia)->name) == pref) { atom_name = pref; break; }
      if (!atom_name.empty()) break;
   }
   if (atom_name.empty()) atom_name = r->GetAtom(0)->name;
   set_go_to_atom_molecule(imol);
   set_go_to_atom_chain_residue_atom_name_full(chain.c_str(), resno, ins.c_str(), atom_name.c_str(), "");
}

void show_list_window(const std::string &title, const std::string &intro, GtkWidget *content, int w, int h); // below

// ------------------------------------------------------------------------------------
//          residue stepping and the sequence strip (inspector)
// ------------------------------------------------------------------------------------

GtkWidget *seq_strip_box = nullptr;
GtkWidget *seq_strip_scroll = nullptr;
std::string seq_strip_signature;
std::vector<std::pair<std::string, GtkWidget *> > seq_strip_cells; // "resno|ins" -> label

char one_letter(const std::string &rn) {
   static const std::map<std::string, char> m = {
      {"ALA",'A'},{"ARG",'R'},{"ASN",'N'},{"ASP",'D'},{"CYS",'C'},{"GLN",'Q'},{"GLU",'E'},{"GLY",'G'},
      {"HIS",'H'},{"ILE",'I'},{"LEU",'L'},{"LYS",'K'},{"MET",'M'},{"PHE",'F'},{"PRO",'P'},{"SER",'S'},
      {"THR",'T'},{"TRP",'W'},{"TYR",'Y'},{"VAL",'V'},{"MSE",'M'},{"SEP",'S'},{"TPO",'T'},{"PTR",'Y'},
      {"A",'A'},{"C",'C'},{"G",'G'},{"U",'U'},{"DA",'A'},{"DC",'C'},{"DG",'G'},{"DT",'T'},{"HOH",'w'}};
   auto it = m.find(rn);
   return it == m.end() ? '*' : it->second;
}

// step to the previous/next residue in the chain of the active residue
void step_residue(int delta) {
   std::pair<bool, std::pair<int, coot::atom_spec_t> > aa = graphics_info_t::active_atom_spec();
   if (!aa.first) { graphics_info_t::add_status_bar_text("Centre on a residue first"); return; }
   int imol = aa.second.first;
   const coot::atom_spec_t &spec = aa.second.second;
   mmdb::Manager *mol = graphics_info_t::molecules[imol].atom_sel.mol;
   if (!mol) return;
   mmdb::Model *model = mol->GetModel(1);
   mmdb::Chain *c = model ? model->GetChain(spec.chain_id.c_str()) : nullptr;
   if (!c) return;
   int n = c->GetNumberOfResidues();
   for (int i=0; i<n; i++) {
      mmdb::Residue *r = c->GetResidue(i);
      if (r && r->GetSeqNum() == spec.res_no && std::string(r->GetInsCode()) == spec.ins_code) {
         int j = i + delta;
         while (j >= 0 && j < n && (!c->GetResidue(j) || c->GetResidue(j)->GetNumberOfAtoms() == 0)) j += delta;
         if (j < 0 || j >= n) { graphics_info_t::add_status_bar_text(delta > 0 ? "End of the chain" : "Start of the chain"); return; }
         mmdb::Residue *rn = c->GetResidue(j);
         go_to_residue(imol, spec.chain_id, rn->GetSeqNum(), rn->GetInsCode());
         return;
      }
   }
}

void update_sequence_strip() {
   if (!seq_strip_box) return;
   std::pair<bool, std::pair<int, coot::atom_spec_t> > aa = graphics_info_t::active_atom_spec();
   if (!aa.first || !is_valid_model_molecule(aa.second.first)) {
      if (!seq_strip_signature.empty()) {
         GtkWidget *c;
         while ((c = gtk_widget_get_first_child(seq_strip_box)) != nullptr) gtk_box_remove(GTK_BOX(seq_strip_box), c);
         seq_strip_cells.clear();
         seq_strip_signature.clear();
      }
      return;
   }
   int imol = aa.second.first;
   const coot::atom_spec_t &spec = aa.second.second;
   mmdb::Manager *mol = graphics_info_t::molecules[imol].atom_sel.mol;
   mmdb::Model *model = mol ? mol->GetModel(1) : nullptr;
   mmdb::Chain *chain = model ? model->GetChain(spec.chain_id.c_str()) : nullptr;
   if (!chain) return;
   std::string sig = std::to_string(imol) + "|" + spec.chain_id + "|" + std::to_string(chain->GetNumberOfResidues()) +
                     "|" + molecule_name(imol);
   if (sig != seq_strip_signature) {
      seq_strip_signature = sig;
      GtkWidget *c;
      while ((c = gtk_widget_get_first_child(seq_strip_box)) != nullptr) gtk_box_remove(GTK_BOX(seq_strip_box), c);
      seq_strip_cells.clear();
      for (int i=0; i<chain->GetNumberOfResidues() && i < 3000; i++) {
         mmdb::Residue *r = chain->GetResidue(i);
         if (!r) continue;
         std::string rn = r->GetResName();
         if (rn == "HOH" || rn == "WAT") continue; // the strip is the sequence
         char ch = one_letter(rn);
         GtkWidget *l = gtk_label_new(std::string(1, ch).c_str());
         gtk_widget_add_css_class(l, "coot-plus-seq-cell");
         if (ch == '*') gtk_widget_add_css_class(l, "coot-plus-seq-het");
         std::string tip = residue_display_name(rn) + " " + std::to_string(r->GetSeqNum()) + r->GetInsCode() + "  chain " + spec.chain_id;
         gtk_widget_set_tooltip_text(l, tip.c_str());
         GtkGesture *click = gtk_gesture_click_new();
         struct cell_t { int imol; std::string chain; int resno; std::string ins; };
         cell_t *ct = new cell_t{imol, spec.chain_id, r->GetSeqNum(), r->GetInsCode()};
         g_object_set_data_full(G_OBJECT(click), "cell", ct, +[] (gpointer d) { delete static_cast<cell_t *>(d); });
         g_signal_connect(click, "released", G_CALLBACK(+[] (GtkGestureClick *g, int, double, double, gpointer) {
                             cell_t *c = static_cast<cell_t *>(g_object_get_data(G_OBJECT(g), "cell"));
                             go_to_residue(c->imol, c->chain, c->resno, c->ins); }), nullptr);
         gtk_widget_add_controller(l, GTK_EVENT_CONTROLLER(click));
         gtk_box_append(GTK_BOX(seq_strip_box), l);
         seq_strip_cells.push_back(std::make_pair(std::to_string(r->GetSeqNum()) + "|" + r->GetInsCode(), l));
      }
   }
   // highlight the current residue and keep it in view
   std::string cur = std::to_string(spec.res_no) + "|" + spec.ins_code;
   for (auto &cell : seq_strip_cells) {
      bool on = cell.first == cur;
      if (on != gtk_widget_has_css_class(cell.second, "coot-plus-seq-current")) {
         if (on) {
            gtk_widget_add_css_class(cell.second, "coot-plus-seq-current");
            graphene_point_t p, origin = GRAPHENE_POINT_INIT(0, 0);
            if (gtk_widget_compute_point(cell.second, seq_strip_box, &origin, &p)) {
               GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(seq_strip_scroll));
               double page = gtk_adjustment_get_page_size(adj);
               gtk_adjustment_set_value(adj, std::max(0.0, p.x - page / 2));
            }
         } else {
            gtk_widget_remove_css_class(cell.second, "coot-plus-seq-current");
         }
      }
   }
}

GtkWidget *make_stepping_row() {
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
   GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
   GtkWidget *prev = gtk_button_new_with_label("◀  Previous");
   GtkWidget *next = gtk_button_new_with_label("Next  ▶");
   for (GtkWidget *b : {prev, next}) { gtk_widget_set_focusable(b, FALSE); gtk_widget_add_css_class(b, "coot-plus-pill"); }
   gtk_widget_set_tooltip_text(prev, "Go to the previous residue in this chain");
   gtk_widget_set_tooltip_text(next, "Go to the next residue in this chain");
   g_signal_connect(prev, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { step_residue(-1); }), nullptr);
   g_signal_connect(next, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { step_residue(+1); }), nullptr);
   gtk_box_append(GTK_BOX(row), prev);
   gtk_box_append(GTK_BOX(row), next);
   seq_strip_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
   seq_strip_scroll = gtk_scrolled_window_new();
   gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(seq_strip_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
   gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(seq_strip_scroll), seq_strip_box);
   gtk_widget_add_css_class(seq_strip_scroll, "coot-plus-seq-strip");
   gtk_widget_set_tooltip_text(seq_strip_scroll, "The sequence of this chain - click a residue to go there");
   gtk_box_append(GTK_BOX(box), row);
   gtk_box_append(GTK_BOX(box), seq_strip_scroll);
   return box;
}

// ------------------------------------------------------------------------------------
//          ligand check card (inspector) - shown when centred on a ligand
// ------------------------------------------------------------------------------------

GtkWidget *ligand_card = nullptr;
GtkWidget *ligand_title = nullptr;
GtkWidget *ligand_details = nullptr;
std::string ligand_signature;

bool is_ligand_residue(const std::string &rn, mmdb::Residue *r) {
   if (rn == "HOH" || rn == "WAT" || rn == "DOD") return false;
   if (one_letter(rn) != '*') return false;              // amino acid / nucleotide
   return r && r->GetNumberOfAtoms() >= 1;
}

// the protein a separate ligand molecule sits in: the biggest other model
int ligand_protein_model(int imol_ligand) {
   int best = -1, n = 0;
   for (int i=0; i<graphics_n_molecules(); i++)
      if (i != imol_ligand && is_valid_model_molecule(i) && mol_is_displayed(i) &&
          graphics_info_t::molecules[i].atom_sel.n_selected_atoms > n) {
         best = i; n = graphics_info_t::molecules[i].atom_sel.n_selected_atoms; }
   return best;
}

void update_ligand_card() {
   if (!ligand_card) return;
   std::pair<bool, std::pair<int, coot::atom_spec_t> > aa = graphics_info_t::active_atom_spec();
   mmdb::Residue *r = nullptr;
   int imol = -1;
   std::string rn;
   if (aa.first && is_valid_model_molecule(aa.second.first)) {
      imol = aa.second.first;
      const coot::atom_spec_t &s = aa.second.second;
      r = graphics_info_t::molecules[imol].get_residue(s.chain_id, s.res_no, s.ins_code);
      if (r) rn = r->GetResName();
   }
   if (!r || !is_ligand_residue(rn, r)) {
      if (gtk_widget_get_visible(ligand_card)) gtk_widget_set_visible(ligand_card, FALSE);
      ligand_signature.clear();
      return;
   }
   const coot::atom_spec_t &s = aa.second.second;
   int imap = best_map();
   std::string sig = std::to_string(imol) + s.chain_id + std::to_string(s.res_no) + s.ins_code + "|" + std::to_string(imap);
   if (sig == ligand_signature) return;
   ligand_signature = sig;
   gtk_widget_set_visible(ligand_card, TRUE);
   double sum_b = 0, sum_occ = 0;
   int n = r->GetNumberOfAtoms();
   for (int i=0; i<n; i++) { sum_b += r->GetAtom(i)->tempFactor; sum_occ += r->GetAtom(i)->occupancy; }
   std::string title = rn + " " + std::to_string(s.res_no) + s.ins_code + "  chain " + s.chain_id;
   gtk_label_set_text(GTK_LABEL(ligand_title), title.c_str());
   std::string d = std::to_string(n) + " atoms · mean B " + format_float(n ? sum_b / n : 0, 1) +
                   " · occ " + format_float(n ? sum_occ / n : 0, 2);
   nlohmann::json info = py_json("__cp_plus.ligand_info(" + std::to_string(imol) + ", " + py_str(s.chain_id) + ", " +
                                 std::to_string(s.res_no) + ", " + py_str(s.ins_code) + ", " + std::to_string(imap) + ", " +
                                 std::to_string(ligand_protein_model(imol)) + ")");
   if (info.is_object()) {
      if (info.contains("cc")) {
         double cc = info["cc"].get<double>();
         d += "\nDensity fit: CC " + format_float(cc, 2) +
              (cc >= 0.8 ? " (good)" : (cc >= 0.6 ? " (fair)" : " (poor)")) + "  [" + display_name(imap) + "]";
      } else {
         d += "\nDensity fit: open a map to see it";
      }
      if (info.contains("contacts")) {
         int nc = 0;
         std::string near;
         for (const auto &c : info["contacts"]) {
            std::string crn = c[3].get<std::string>();
            if (crn == "HOH" || crn == "WAT") continue;
            nc++;
            if (nc <= 5) near += (near.empty() ? "" : ", ") + residue_display_name(crn) + std::to_string(c[1].get<int>());
         }
         d += "\nWithin 4 Å: " + (near.empty() ? std::string("no residues") : near + (nc > 5 ? " …" : "")) +
              " · " + std::to_string(info.value("n_waters_near", 0)) + " waters";
      }
   }
   gtk_label_set_text(GTK_LABEL(ligand_details), d.c_str());
}

GtkWidget *make_ligand_card(GtkWidget *parent) {
   GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_add_css_class(card, "coot-plus-card");
   gtk_widget_add_css_class(card, "coot-plus-ligand-card");
   gtk_box_append(GTK_BOX(card), section_label("Ligand"));
   ligand_title = gtk_label_new("");
   gtk_label_set_xalign(GTK_LABEL(ligand_title), 0.0);
   gtk_widget_add_css_class(ligand_title, "coot-plus-selected-title");
   ligand_details = gtk_label_new("");
   gtk_label_set_xalign(GTK_LABEL(ligand_details), 0.0);
   gtk_label_set_wrap(GTK_LABEL(ligand_details), TRUE);
   gtk_label_set_max_width_chars(GTK_LABEL(ligand_details), 32);
   gtk_widget_add_css_class(ligand_details, "coot-plus-dim");
   gtk_box_append(GTK_BOX(card), ligand_title);
   gtk_box_append(GTK_BOX(card), ligand_details);
   GtkWidget *grid = gtk_grid_new();
   gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
   gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
   gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
   const std::vector<std::pair<std::string, std::string> > tools = {
      {"kb:Sphere Refine", "Refine"}, {"app.jiggle_fit_active_residue_action", "Jiggle Fit"},
      {"app.geometric_distortions_for_ligand_action", "Distortions"}, {"app.coot_contact_dots_for_ligand_action", "Contacts"},
      {"app.environment_distances_action", "Distances"}, {"app.flev_action", "2D View"}};
   int i = 0;
   for (const auto &t : tools) {
      const command_t *c = resolve_tool(t.first);
      if (!c) continue;
      GtkWidget *b = gtk_button_new_with_label(t.second.c_str());
      gtk_widget_add_css_class(b, "coot-plus-pill");
      gtk_widget_set_focusable(b, FALSE);
      gtk_widget_set_tooltip_text(b, tooltip_for(*c).c_str());
      command_t *copy = new command_t(*c);
      g_signal_connect_data(b, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer d) {
                               run_command(*static_cast<command_t *>(d)); ligand_signature.clear(); }),
                            copy, +[] (gpointer d, GClosure *) { delete static_cast<command_t *>(d); }, GConnectFlags(0));
      gtk_grid_attach(GTK_GRID(grid), b, i % 3, i / 3, 1, 1);
      i++;
   }
   gtk_box_append(GTK_BOX(card), grid);
   gtk_widget_set_visible(card, FALSE);
   gtk_box_append(GTK_BOX(parent), card);
   ligand_card = card;
   return card;
}

// ------------------------------------------------------------------------------------
//          list windows (blobs, problems)
// ------------------------------------------------------------------------------------

void show_list_window(const std::string &title, const std::string &intro, GtkWidget *content, int w, int h) {
   GtkWidget *win = gtk_window_new();
   gtk_window_set_title(GTK_WINDOW(win), title.c_str());
   gtk_window_set_default_size(GTK_WINDOW(win), w, h);
   if (GtkWidget *mw = graphics_info_t::get_main_window()) gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(mw));
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
   gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12);
   gtk_widget_set_margin_top(box, 10);   gtk_widget_set_margin_bottom(box, 12);
   GtkWidget *l = gtk_label_new(intro.c_str());
   gtk_label_set_xalign(GTK_LABEL(l), 0.0);
   gtk_label_set_wrap(GTK_LABEL(l), TRUE);
   gtk_widget_add_css_class(l, "coot-plus-dim");
   GtkWidget *sw = gtk_scrolled_window_new();
   gtk_widget_set_vexpand(sw, TRUE);
   gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), content);
   gtk_box_append(GTK_BOX(box), l);
   gtk_box_append(GTK_BOX(box), sw);
   gtk_window_set_child(GTK_WINDOW(win), box);
   gtk_window_present(GTK_WINDOW(win));
}

GtkWidget *list_row_button(const std::string &title, const std::string &subtitle, const std::string &css_extra) {
   GtkWidget *b = gtk_button_new();
   GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
   GtkWidget *t = gtk_label_new(title.c_str());
   GtkWidget *st = gtk_label_new(subtitle.c_str());
   gtk_label_set_xalign(GTK_LABEL(t), 0.0);
   gtk_label_set_xalign(GTK_LABEL(st), 0.0);
   gtk_label_set_wrap(GTK_LABEL(st), TRUE);
   gtk_widget_add_css_class(t, "coot-plus-map-name");
   gtk_widget_add_css_class(st, "coot-plus-dim");
   if (!css_extra.empty()) gtk_widget_add_css_class(t, css_extra.c_str());
   gtk_box_append(GTK_BOX(v), t);
   gtk_box_append(GTK_BOX(v), st);
   gtk_button_set_child(GTK_BUTTON(b), v);
   gtk_widget_add_css_class(b, "flat");
   gtk_widget_add_css_class(b, "coot-plus-tool");
   gtk_widget_set_focusable(b, FALSE);
   return b;
}

void show_blobs_window() {
   int imol = best_model(), imap = best_map();
   if (imol < 0 || imap < 0) {
      coot_plus_message("warning", "Unmodelled blobs needs a model and a map", "Open a model, then Auto Open MTZ");
      return;
   }
   graphics_info_t::add_status_bar_text("Looking for unmodelled density…");
   nlohmann::json bl = py_json("__cp_plus.blobs(" + std::to_string(imol) + ", " + std::to_string(imap) + ", 1.0)");
   GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
   int n = 0;
   if (bl.is_array()) {
      for (const auto &b : bl) {
         n++;
         std::string near;
         for (const auto &r : b["near"])
            near += (near.empty() ? "" : ", ") + r[0].get<std::string>() + std::to_string(r[1].get<int>()) + " " +
                    residue_display_name(r[3].get<std::string>());
         std::string title = "Blob " + std::to_string(n) + "   ≈" + format_float(b["score"].get<double>(), 0) + " Å³";
         std::string sub = near.empty() ? "No residues within 5 Å" : "Near " + near;
         GtkWidget *row = list_row_button(title, sub, "");
         struct pos_t { double x, y, z; };
         pos_t *p = new pos_t{b["x"].get<double>(), b["y"].get<double>(), b["z"].get<double>()};
         g_signal_connect_data(row, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer d) {
                                  pos_t *p = static_cast<pos_t *>(d); set_rotation_centre(p->x, p->y, p->z); }),
                               p, +[] (gpointer d, GClosure *) { delete static_cast<pos_t *>(d); }, GConnectFlags(0));
         gtk_box_append(GTK_BOX(list), row);
      }
   }
   if (n == 0) {
      GtkWidget *l = gtk_label_new("No unmodelled blobs found at 1.0 σ.");
      gtk_box_append(GTK_BOX(list), l);
   }
   show_list_window("Unmodelled Blobs - " + display_name(imol),
                    std::to_string(n) + " regions of density (above 1.0 σ in " + display_name(imap) +
                    ") that the model doesn't explain and that are too big to be a water - biggest first. "
                    "Volume is the size of the density cluster. Click one to go there.", list, 460, 520);
   graphics_info_t::add_status_bar_text(std::to_string(n) + " unmodelled blobs");
}

// ------------------------------------------------------------------------------------
//          validation: problems drawn on the model, and a list
// ------------------------------------------------------------------------------------

std::set<int> models_with_markers;

void show_problems_on_model(bool on) {
   int imol = best_model();
   if (imol < 0) { graphics_info_t::add_status_bar_text("Open a model first"); return; }
   if (!on) {
      py_json("__cp_plus.clear_validation_markers(" + std::to_string(imol) + ")");
      models_with_markers.erase(imol);
      graphics_info_t::add_status_bar_text("Problem markers removed");
      graphics_draw();
      return;
   }
   nlohmann::json r = py_json("__cp_plus.draw_validation_markers(" + std::to_string(imol) + ", " + std::to_string(best_map()) + ")");
   models_with_markers.insert(imol);
   int n = (r.is_array() && r.size() > 1) ? r[1].get<int>() : 0;
   graphics_info_t::add_status_bar_text("Marked " + std::to_string(n) + " residues with problems: red = serious, orange = minor");
   graphics_draw();
}

void show_problems_window() {
   int imol = best_model(), imap = best_map();
   if (imol < 0) { graphics_info_t::add_status_bar_text("Open a model first"); return; }
   nlohmann::json issues = py_json("__cp_plus.validation_issues(" + std::to_string(imol) + ", " + std::to_string(imap) + ")");
   GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
   int n = 0;
   if (issues.is_array()) {
      for (const auto &d : issues) {
         n++;
         std::string title = residue_display_name(d.value("resname", "")) + " " + std::to_string(d["resno"].get<int>()) +
                             d.value("ins", "") + "  chain " + d.value("chain", "");
         std::string sub;
         for (const auto &p : d["problems"]) sub += (sub.empty() ? "" : " · ") + p.get<std::string>();
         GtkWidget *row = list_row_button((d["severity"].get<int>() >= 2 ? "● " : "○ ") + title, sub,
                                          d["severity"].get<int>() >= 2 ? "coot-plus-serious" : "coot-plus-minor");
         struct res_t { int imol; std::string chain; int resno; std::string ins; };
         res_t *rr = new res_t{imol, d.value("chain", ""), d["resno"].get<int>(), d.value("ins", "")};
         g_signal_connect_data(row, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer p) {
                                  res_t *r = static_cast<res_t *>(p); go_to_residue(r->imol, r->chain, r->resno, r->ins); }),
                               rr, +[] (gpointer p, GClosure *) { delete static_cast<res_t *>(p); }, GConnectFlags(0));
         gtk_box_append(GTK_BOX(list), row);
      }
   }
   if (n == 0) gtk_box_append(GTK_BOX(list), gtk_label_new("No problems found."));
   show_list_window("Problems - " + display_name(imol),
                    std::to_string(n) + " residues: Ramachandran (Coot's plot thresholds), rotamers below 1%" +
                    std::string(imap >= 0 ? ", and density fit (CC below 0.6 with " + display_name(imap) + ")" : " (open a map to check density fit too)") +
                    ". ● serious, ○ minor. Click one to go there.", list, 480, 560);
}

// ------------------------------------------------------------------------------------
//          Phenix refinement runs
// ------------------------------------------------------------------------------------

std::string phenix_folder;
double phenix_opened_mtime = 0;
guint phenix_watch = 0;

void open_phenix_run(const nlohmann::json &run) {
   py_json("__cp_plus.open_phenix_run(" + py_str(run["model"].get<std::string>()) + ", " + py_str(run["mtz"].get<std::string>()) + ")");
   phenix_opened_mtime = run["mtime"].get<double>();
   graphics_info_t::add_status_bar_text("Opened Phenix run " + std::to_string(run["run"].get<int>()) + ": " +
                                        file_basename(run["model"].get<std::string>()));
   refresh_phenix_box();
}

gboolean phenix_watch_tick(gpointer) {
   if (phenix_folder.empty()) return G_SOURCE_CONTINUE;
   nlohmann::json runs = py_json("__cp_plus.phenix_runs(" + py_str(phenix_folder) + ")");
   if (runs.is_array() && !runs.empty()) {
      const auto &newest = runs.back();
      if (newest["mtime"].get<double>() > phenix_opened_mtime + 1.0) {
         static double told = 0;
         if (newest["mtime"].get<double>() != told) {
            told = newest["mtime"].get<double>();
            coot_plus_message("info", "Newer Phenix refinement: run " + std::to_string(newest["run"].get<int>()) + " (" +
                              file_basename(newest["model"].get<std::string>()) + ")",
                              "Open it from Load › Phenix");
            refresh_phenix_box();
         }
      }
   }
   return G_SOURCE_CONTINUE;
}

void use_phenix_folder(const std::string &folder) {
   phenix_folder = folder;
   GKeyFile *kf = g_key_file_new();
   std::string fn = coot_plus_settings_file();
   g_key_file_load_from_file(kf, fn.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
   g_key_file_set_string(kf, "interface", "phenix-folder", folder.c_str());
   gchar *dir = g_path_get_dirname(fn.c_str()); g_mkdir_with_parents(dir, 0755); g_free(dir);
   g_key_file_save_to_file(kf, fn.c_str(), nullptr);
   g_key_file_free(kf);
   nlohmann::json runs = py_json("__cp_plus.phenix_runs(" + py_str(folder) + ")");
   if (!runs.is_array() || runs.empty()) {
      coot_plus_message("warning", "No Phenix refinement runs found in " + file_basename(folder),
                        "Looking for *_refine_NNN.pdb (or .cif) with a matching .mtz");
      refresh_phenix_box();
      return;
   }
   nlohmann::json newest = runs.back();
   ask_choice("Open Phenix run " + std::to_string(newest["run"].get<int>()) + "?",
              file_basename(newest["model"].get<std::string>()) + " and its maps (2mFo-DFc, mFo-DFc).\n"
              "Close what's open first? (Maps from a different run won't match.)",
              {"Cancel", "Add to Open Files", "Close Others and Open"}, 0, 2, [newest] (int r) {
                 if (r == 2)
                    for (int i=0; i<graphics_n_molecules(); i++)
                       if (is_valid_model_molecule(i) || is_valid_map_molecule(i)) close_molecule(i);
                 open_phenix_run(newest);
              });
   if (!phenix_watch) phenix_watch = g_timeout_add_seconds(20, phenix_watch_tick, nullptr);
}

void choose_phenix_folder() {
   GtkFileDialog *fd = gtk_file_dialog_new();
   gtk_file_dialog_set_title(fd, "Choose a Phenix refinement folder");
   if (!phenix_folder.empty()) {
      GFile *f = g_file_new_for_path(phenix_folder.c_str());
      gtk_file_dialog_set_initial_folder(fd, f);
      g_object_unref(f);
   }
   GtkWidget *mw = graphics_info_t::get_main_window();
   gtk_file_dialog_select_folder(fd, mw ? GTK_WINDOW(mw) : nullptr, nullptr, +[] (GObject *src, GAsyncResult *res, gpointer) {
                                    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, nullptr);
                                    if (!f) return;
                                    char *p = g_file_get_path(f);
                                    if (p) use_phenix_folder(p);
                                    g_free(p);
                                    g_object_unref(f);
                                 }, nullptr);
   g_object_unref(fd);
}

void refresh_phenix_box() {
   if (!phenix_box) return;
   GtkWidget *c;
   while ((c = gtk_widget_get_first_child(phenix_box)) != nullptr) gtk_box_remove(GTK_BOX(phenix_box), c);
   GtkWidget *choose = gtk_button_new();
   gtk_button_set_child(GTK_BUTTON(choose), tool_button_content("coot-plus-load-symbolic", "Open Phenix Refinement…", ""));
   gtk_widget_add_css_class(choose, "flat");
   gtk_widget_add_css_class(choose, "coot-plus-tool");
   gtk_widget_set_focusable(choose, FALSE);
   gtk_widget_set_tooltip_text(choose, "Choose a Phenix refinement folder: opens the newest *_refine_NNN model with its "
                                       "2mFo-DFc and mFo-DFc maps, and tells you when a newer run appears");
   g_signal_connect(choose, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { choose_phenix_folder(); }), nullptr);
   gtk_box_append(GTK_BOX(phenix_box), choose);
   if (!phenix_folder.empty()) {
      nlohmann::json runs = py_json("__cp_plus.phenix_runs(" + py_str(phenix_folder) + ")");
      if (runs.is_array() && !runs.empty()) {
         const nlohmann::json newest = runs.back();
         bool newer = newest["mtime"].get<double>() > phenix_opened_mtime + 1.0;
         std::string t = std::string(newer ? "★ Newest: " : "Newest: ") + "run " + std::to_string(newest["run"].get<int>());
         GtkWidget *b = make_file_row("map", t, file_basename(phenix_folder) + " · " + std::to_string(runs.size()) + " runs",
                                      newest["model"].get<std::string>() + "\n" + newest["mtz"].get<std::string>() +
                                      "\n\nClick to open (asks whether to close what's open)");
         nlohmann::json *copy = new nlohmann::json(newest);
         g_signal_connect_data(b, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer d) {
                                  nlohmann::json run = *static_cast<nlohmann::json *>(d);
                                  ask_choice("Open Phenix run " + std::to_string(run["run"].get<int>()) + "?", "Close what's open first?",
                                             {"Cancel", "Add to Open Files", "Close Others and Open"}, 0, 2, [run] (int r) {
                                                if (r == 2)
                                                   for (int i=0; i<graphics_n_molecules(); i++)
                                                      if (is_valid_model_molecule(i) || is_valid_map_molecule(i)) close_molecule(i);
                                                open_phenix_run(run); }); }),
                               copy, +[] (gpointer d, GClosure *) { delete static_cast<nlohmann::json *>(d); }, GConnectFlags(0));
         gtk_box_append(GTK_BOX(phenix_box), b);
      }
   }
}


// ------------------------------------------------------------------------------------
//          Find Ligands with sensible defaults
// ------------------------------------------------------------------------------------

GtkWidget *molecule_dropdown(const std::vector<int> &mols, int selected) {
   std::vector<std::string> names;
   for (int i : mols) names.push_back(display_name(i) + "  (#" + std::to_string(i) + ")");
   std::vector<const char *> c;
   for (const auto &n : names) c.push_back(n.c_str());
   c.push_back(nullptr);
   GtkWidget *dd = gtk_drop_down_new_from_strings(c.data());
   for (unsigned int k=0; k<mols.size(); k++) if (mols[k] == selected) gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), k);
   return dd;
}

void show_find_ligands_window() {
   std::vector<int> proteins, ligands, maps;
   for (int i=0; i<graphics_n_molecules(); i++) {
      if (is_valid_model_molecule(i)) {
         int n = graphics_info_t::molecules[i].atom_sel.n_selected_atoms;
         if (n < 300) ligands.push_back(i); else proteins.push_back(i);
      } else if (is_valid_map_molecule(i) && !map_is_difference_map(i)) maps.push_back(i);
   }
   if (proteins.empty() || maps.empty()) {
      coot_plus_message("warning", "Find Ligands needs a model and a map",
                        "Open the model, then Auto Open MTZ (or Load › Phenix)");
      return;
   }
   GtkWidget *win = gtk_window_new();
   gtk_window_set_title(GTK_WINDOW(win), "Find Ligands");
   gtk_window_set_default_size(GTK_WINDOW(win), 420, -1);
   if (GtkWidget *mw = graphics_info_t::get_main_window()) gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(mw));
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
   gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
   gtk_widget_set_margin_top(box, 14);   gtk_widget_set_margin_bottom(box, 14);
   auto row = [&] (const char *label, GtkWidget *w, const char *tip) {
      GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
      GtkWidget *l = gtk_label_new(label);
      gtk_label_set_xalign(GTK_LABEL(l), 0.0);
      gtk_widget_set_size_request(l, 90, -1);
      gtk_widget_set_hexpand(w, TRUE);
      if (tip) gtk_widget_set_tooltip_text(r, tip);
      gtk_box_append(GTK_BOX(r), l);
      gtk_box_append(GTK_BOX(r), w);
      gtk_box_append(GTK_BOX(box), r);
   };
   GtkWidget *intro = gtk_label_new("Fits a ligand into density the model doesn't explain. The map and protein are chosen "
                                    "for you; check them and press Find.");
   gtk_label_set_wrap(GTK_LABEL(intro), TRUE);
   gtk_label_set_xalign(GTK_LABEL(intro), 0.0);
   gtk_widget_add_css_class(intro, "coot-plus-dim");
   gtk_box_append(GTK_BOX(box), intro);

   GtkWidget *lig_dd = nullptr;
   if (ligands.empty()) {
      GtkWidget *get = gtk_button_new_with_label("Get Monomer… (load the ligand first)");
      g_signal_connect(get, "clicked", G_CALLBACK(+[] (GtkButton *b, gpointer) {
                          g_action_group_activate_action(G_ACTION_GROUP(coot_plus_app), "get_monomer_action", nullptr);
                          gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b))));
                          graphics_info_t::add_status_bar_text("After the ligand is loaded, open Find Ligands again");
                       }), nullptr);
      row("Ligand", get, "No small molecule is open yet: get it by its three-letter code (e.g. GOL, ATP)");
   } else {
      lig_dd = molecule_dropdown(ligands, ligands.back());
      row("Ligand", lig_dd, "The ligand to fit (an open molecule with fewer than 300 atoms)");
   }
   GtkWidget *map_dd = molecule_dropdown(maps, best_map());
   row("Map", map_dd, "Fit into this map - usually the 2mFo-DFc map");
   GtkWidget *prot_dd = molecule_dropdown(proteins, best_model() >= 0 && graphics_info_t::molecules[best_model()].atom_sel.n_selected_atoms >= 300
                                                    ? best_model() : proteins[0]);
   row("Protein", prot_dd, "Density already explained by this model is not used");
   GtkWidget *here = gtk_check_button_new_with_label("Only right here (the blob at the screen centre)");
   gtk_check_button_set_active(GTK_CHECK_BUTTON(here), TRUE);
   gtk_widget_set_tooltip_text(here, "Untick to search the whole map (slower)");
   GtkWidget *flex = gtk_check_button_new_with_label("Flexible ligand (try conformations)");
   gtk_widget_set_tooltip_text(flex, "Also samples the ligand's rotatable bonds - slower, better for long ligands");
   gtk_box_append(GTK_BOX(box), here);
   gtk_box_append(GTK_BOX(box), flex);
   GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_widget_set_halign(buttons, GTK_ALIGN_END);
   GtkWidget *cancel = gtk_button_new_with_label("Cancel");
   GtkWidget *go = gtk_button_new_with_label("Find");
   gtk_widget_add_css_class(go, "suggested-action");
   gtk_widget_set_sensitive(go, lig_dd != nullptr);
   gtk_box_append(GTK_BOX(buttons), cancel);
   gtk_box_append(GTK_BOX(buttons), go);
   gtk_box_append(GTK_BOX(box), buttons);
   gtk_window_set_child(GTK_WINDOW(win), box);
   g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), win);

   struct fl_t { std::vector<int> ligands, maps, proteins; GtkWidget *lig, *map, *prot, *here, *flex; };
   fl_t *ctx = new fl_t{ligands, maps, proteins, lig_dd, map_dd, prot_dd, here, flex};
   g_object_set_data_full(G_OBJECT(win), "ctx", ctx, +[] (gpointer d) { delete static_cast<fl_t *>(d); });
   g_signal_connect(go, "clicked", G_CALLBACK(+[] (GtkButton *b, gpointer w) {
                       fl_t *c = static_cast<fl_t *>(g_object_get_data(G_OBJECT(w), "ctx"));
                       int lig  = c->ligands [gtk_drop_down_get_selected(GTK_DROP_DOWN(c->lig))];
                       int map  = c->maps    [gtk_drop_down_get_selected(GTK_DROP_DOWN(c->map))];
                       int prot = c->proteins[gtk_drop_down_get_selected(GTK_DROP_DOWN(c->prot))];
                       bool here = gtk_check_button_get_active(GTK_CHECK_BUTTON(c->here));
                       bool flex = gtk_check_button_get_active(GTK_CHECK_BUTTON(c->flex));
                       gtk_window_destroy(GTK_WINDOW(w));
                       graphics_info_t::add_status_bar_text("Fitting " + display_name(lig) + "…");
                       add_ligand_clear_ligands();
                       set_ligand_search_protein_molecule(prot);
                       set_ligand_search_map_molecule(map);
                       if (flex) add_ligand_search_wiggly_ligand_molecule(lig); else add_ligand_search_ligand_molecule(lig);
                       set_find_ligand_here_cluster(here ? 1 : 0);
                       nlohmann::json sol = py_json("__cp_coot.execute_ligand_search_py()");
                       set_find_ligand_here_cluster(0);
                       // Coot shows its own result dialog too - Coot+ reports the result itself
                       for (const char *id : {"new_ligands_info_dialog", "no_new_ligands_info_dialog"})
                          if (GtkWidget *d = widget_from_builder(id)) gtk_widget_set_visible(d, FALSE);
                       int n = sol.is_array() ? sol.size() : 0;
                       if (n == 0) {
                          coot_plus_message("warning", "No place found for " + display_name(lig),
                                            here ? "Centre on the blob (Option-click) and try again, or untick “Only right here”"
                                                 : "Try a lower contour, or check the ligand matches the density");
                          return;
                       }
                       int best = sol[0].get<int>();
                       std::string all;
                       for (const auto &x : sol) all += (all.empty() ? "#" : ", #") + std::to_string(x.get<int>());
                       ask_choice("Placed " + display_name(lig) + ": " + std::to_string(n) + " solution(s)",
                                  "The best is #" + std::to_string(best) + " (now centred). Solutions: " + all +
                                  ".\nMerge the best one into " + display_name(prot) + "?",
                                  {"Keep Separate", "Merge Best into Model"}, 0, 1, [best, prot] (int) {
                                     py_json("__cp_coot.merge_molecules_py([" + std::to_string(best) + "], " + std::to_string(prot) + ")");
                                     close_molecule(best);
                                     graphics_info_t::add_status_bar_text("Merged the ligand into " + display_name(prot));
                                  });
                       (void) b;
                    }), win);
   gtk_window_present(GTK_WINDOW(win));
}

// ------------------------------------------------------------------------------------
//          Write assembly as chains: symmetry copies that touch the model
// ------------------------------------------------------------------------------------

struct sym_copy_t { int symop; int a, b, c; int contacts; mmdb::mat44 m; std::string name; };

std::vector<sym_copy_t> contacting_copies(int imol, double contact_dist) {
   std::vector<sym_copy_t> out;
   mmdb::Manager *mol = graphics_info_t::molecules[imol].atom_sel.mol;
   if (!mol) return out;
   // representative atoms: CA / P / C1' (all atoms for small molecules)
   std::vector<clipper::Coord_orth> pts;
   const atom_selection_container_t &asc = graphics_info_t::molecules[imol].atom_sel;
   for (int i=0; i<asc.n_selected_atoms; i++) {
      mmdb::Atom *at = asc.atom_selection[i];
      std::string n = at->name;
      if (n == " CA " || n == " P  " || n == " C1'" || asc.n_selected_atoms < 400)
         pts.push_back(clipper::Coord_orth(at->x, at->y, at->z));
   }
   if (pts.empty()) return out;
   clipper::Coord_orth centre(0,0,0);
   for (const auto &p : pts) centre += p;
   centre = (1.0 / pts.size()) * centre;
   double radius = 0;
   for (const auto &p : pts) radius = std::max(radius, clipper::Coord_orth::length(p, centre));
   int nsym = mol->GetNumberOfSymOps();
   double d2 = contact_dist * contact_dist;
   for (int isym=0; isym<nsym; isym++) {
      for (int a=-3; a<=3; a++) for (int b=-3; b<=3; b++) for (int c=-3; c<=3; c++) {
         if (isym == 0 && a == 0 && b == 0 && c == 0) continue;
         sym_copy_t sc;
         if (mol->GetTMatrix(sc.m, isym, a, b, c) != 0) continue;
         auto tf = [&] (const clipper::Coord_orth &p) {
            return clipper::Coord_orth(sc.m[0][0]*p.x() + sc.m[0][1]*p.y() + sc.m[0][2]*p.z() + sc.m[0][3],
                                       sc.m[1][0]*p.x() + sc.m[1][1]*p.y() + sc.m[1][2]*p.z() + sc.m[1][3],
                                       sc.m[2][0]*p.x() + sc.m[2][1]*p.y() + sc.m[2][2]*p.z() + sc.m[2][3]);
         };
         if (clipper::Coord_orth::length(tf(centre), centre) > 2 * radius + contact_dist) continue;
         int n = 0;
         for (const auto &p : pts) {
            clipper::Coord_orth q = tf(p);
            for (const auto &r : pts) if ((q - r).lengthsq() < d2) { n++; break; }
         }
         if (n >= 3) {
            sc.symop = isym; sc.a = a; sc.b = b; sc.c = c; sc.contacts = n;
            const char *op = mol->GetSymOp(isym);
            sc.name = std::string(op ? op : "?");
            if (a || b || c) sc.name += "  + (" + std::to_string(a) + "," + std::to_string(b) + "," + std::to_string(c) + ")";
            out.push_back(sc);
         }
      }
   }
   std::sort(out.begin(), out.end(), [] (const sym_copy_t &x, const sym_copy_t &y) { return x.contacts > y.contacts; });
   return out;
}

void show_assembly_window() {
   int imol = best_model();
   if (imol < 0) { graphics_info_t::add_status_bar_text("Open a model first"); return; }
   if (model_cell(imol).size() < 6) {
      coot_plus_message("warning", display_name(imol) + " has no unit cell (CRYST1)", "Symmetry copies need the cell and space group");
      return;
   }
   graphics_info_t::add_status_bar_text("Looking for symmetry copies that touch " + display_name(imol) + "…");
   std::vector<sym_copy_t> copies = contacting_copies(imol, 8.0);
   if (copies.empty()) {
      coot_plus_message("info", "No symmetry copy touches " + display_name(imol), "It has no crystal contacts within 8 Å (CA-CA)");
      return;
   }
   GtkWidget *win = gtk_window_new();
   gtk_window_set_title(GTK_WINDOW(win), "Make Assembly from Symmetry");
   gtk_window_set_default_size(GTK_WINDOW(win), 460, 480);
   if (GtkWidget *mw = graphics_info_t::get_main_window()) gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(mw));
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
   gtk_widget_set_margin_start(box, 14); gtk_widget_set_margin_end(box, 14);
   gtk_widget_set_margin_top(box, 12);   gtk_widget_set_margin_bottom(box, 12);
   GtkWidget *intro = gtk_label_new(("Symmetry copies of " + display_name(imol) + " that touch it (CA atoms within 8 Å), most "
                                     "contacts first. Tick the ones that form your assembly (e.g. the other two subunits of a "
                                     "trimer) - a new model is made with the copies as extra chains.").c_str());
   gtk_label_set_wrap(GTK_LABEL(intro), TRUE);
   gtk_label_set_xalign(GTK_LABEL(intro), 0.0);
   gtk_widget_add_css_class(intro, "coot-plus-dim");
   gtk_box_append(GTK_BOX(box), intro);
   GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
   std::vector<GtkWidget *> checks;
   for (unsigned int i=0; i<copies.size() && i<24; i++) {
      std::string t = copies[i].name + "    " + std::to_string(copies[i].contacts) + " CA in contact";
      GtkWidget *cb = gtk_check_button_new_with_label(t.c_str());
      gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), i < 2 && copies[i].contacts >= copies[0].contacts / 3);
      gtk_box_append(GTK_BOX(list), cb);
      checks.push_back(cb);
   }
   GtkWidget *sw = gtk_scrolled_window_new();
   gtk_widget_set_vexpand(sw, TRUE);
   gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), list);
   gtk_box_append(GTK_BOX(box), sw);
   GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_widget_set_halign(buttons, GTK_ALIGN_END);
   GtkWidget *preview = gtk_button_new_with_label("Show Symmetry");
   gtk_widget_set_tooltip_text(preview, "Turn the symmetry display on to look at the neighbours first");
   GtkWidget *cancel = gtk_button_new_with_label("Cancel");
   GtkWidget *make = gtk_button_new_with_label("Make Assembly Model");
   gtk_widget_add_css_class(make, "suggested-action");
   gtk_box_append(GTK_BOX(buttons), preview);
   gtk_box_append(GTK_BOX(buttons), cancel);
   gtk_box_append(GTK_BOX(buttons), make);
   gtk_box_append(GTK_BOX(box), buttons);
   gtk_window_set_child(GTK_WINDOW(win), box);
   g_signal_connect(preview, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { set_symmetry_view(true); }), nullptr);
   g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), win);
   struct as_t { int imol; std::vector<sym_copy_t> copies; std::vector<GtkWidget *> checks; };
   as_t *ctx = new as_t{imol, copies, checks};
   g_object_set_data_full(G_OBJECT(win), "ctx", ctx, +[] (gpointer d) { delete static_cast<as_t *>(d); });
   g_signal_connect(make, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer w) {
                       as_t *c = static_cast<as_t *>(g_object_get_data(G_OBJECT(w), "ctx"));
                       std::vector<int> made;
                       for (unsigned int i=0; i<c->checks.size(); i++) {
                          if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(c->checks[i]))) continue;
                          const mmdb::mat44 &m = c->copies[i].m;
                          int ic = new_molecule_by_symmetry(c->imol, "", m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2],
                                                            m[2][0], m[2][1], m[2][2], m[0][3], m[1][3], m[2][3], 0, 0, 0);
                          if (ic >= 0) made.push_back(ic);
                       }
                       int imol = c->imol;
                       std::string orig = display_name(imol);
                       gtk_window_destroy(GTK_WINDOW(w));
                       if (made.empty()) { graphics_info_t::add_status_bar_text("Nothing ticked"); return; }
                       int base = copy_molecule(imol);
                       std::string list;
                       for (int m : made) list += (list.empty() ? "" : ", ") + std::to_string(m);
                       nlohmann::json r = py_json("__cp_coot.merge_molecules_py([" + list + "], " + std::to_string(base) + ")");
                       for (int m : made) close_molecule(m);
                       std::string chains;
                       if (r.is_array()) for (unsigned int k=1; k<r.size(); k++) if (r[k].is_string()) chains += (chains.empty() ? "" : ", ") + r[k].get<std::string>();
                       set_custom_label(base, "Assembly of " + orig);
                       graphics_info_t::add_status_bar_text("Made “Assembly of " + orig + "” (#" + std::to_string(base) + ")" +
                                                            (chains.empty() ? "" : " - new chains " + chains) +
                                                            ". Save it with Save › Save Coordinates.");
                       graphics_draw();
                    }), win);
   gtk_window_present(GTK_WINDOW(win));
}


// ------------------------------------------------------------------------------------
//          appearance: light/dark, accent colour, 3D background
// ------------------------------------------------------------------------------------

GtkCssProvider *accent_provider = nullptr;

const std::vector<std::pair<std::string, std::string> > &accent_colours() {
   static const std::vector<std::pair<std::string, std::string> > v = {
      {"Blue", "#3584e4"}, {"Teal", "#2190a4"}, {"Green", "#2ec27e"}, {"Orange", "#e66100"},
      {"Pink", "#d6409f"}, {"Purple", "#9141ac"}};
   return v;
}

std::string load_coot_plus_setting_string(const char *key, const std::string &dflt) {
   std::string r = dflt;
   GKeyFile *kf = g_key_file_new();
   if (g_key_file_load_from_file(kf, coot_plus_settings_file().c_str(), G_KEY_FILE_NONE, nullptr)) {
      gchar *v = g_key_file_get_string(kf, "interface", key, nullptr);
      if (v) { r = v; g_free(v); }
   }
   g_key_file_free(kf);
   return r;
}

void save_coot_plus_setting_string(const char *key, const std::string &value) {
   std::string fn = coot_plus_settings_file();
   GKeyFile *kf = g_key_file_new();
   g_key_file_load_from_file(kf, fn.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
   g_key_file_set_string(kf, "interface", key, value.c_str());
   gchar *dir = g_path_get_dirname(fn.c_str()); g_mkdir_with_parents(dir, 0755); g_free(dir);
   g_key_file_save_to_file(kf, fn.c_str(), nullptr);
   g_key_file_free(kf);
}

void apply_accent(const std::string &hex) {
   if (!accent_provider) {
      accent_provider = gtk_css_provider_new();
      gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(accent_provider),
                                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
   }
   // (named colours don't cross style providers, so every accent rule lives here)
   std::string css = ".coot-plus-task-list row:selected { background-color: alpha(" + hex + ", 0.85); color: #ffffff; }\n"
                     ".coot-plus-dm-button { background-color: alpha(" + hex + ", 0.9); color: #ffffff; }\n"
                     ".coot-plus-seq-current { background-color: " + hex + "; color: #ffffff; }\n"
                     ".coot-plus-msg-info { background-color: alpha(" + hex + ", 0.20); }\n"
                     ".coot-plus-header .suggested-action, window .suggested-action { background-color: " + hex + "; color: #ffffff; }\n";
   gtk_css_provider_load_from_string(accent_provider, css.c_str());
}

void apply_theme(const std::string &theme) { // "dark", "light" or "system"
   GtkSettings *st = gtk_settings_get_default();
   if (!st) return;
   GtkInterfaceColorScheme cs = GTK_INTERFACE_COLOR_SCHEME_DEFAULT;
   if (theme == "dark")  cs = GTK_INTERFACE_COLOR_SCHEME_DARK;
   if (theme == "light") cs = GTK_INTERFACE_COLOR_SCHEME_LIGHT;
   if (g_object_class_find_property(G_OBJECT_GET_CLASS(st), "gtk-interface-color-scheme"))
      g_object_set(st, "gtk-interface-color-scheme", cs, NULL);
   // Coot sets the older flag at start-up, which would win otherwise
   if (g_object_class_find_property(G_OBJECT_GET_CLASS(st), "gtk-application-prefer-dark-theme"))
      g_object_set(st, "gtk-application-prefer-dark-theme", theme != "light", NULL);
}

void set_theme(const std::string &theme) { apply_theme(theme); save_coot_plus_setting_string("theme", theme); }
void set_accent(const std::string &hex)  { apply_accent(hex);  save_coot_plus_setting_string("accent", hex); }

// ------------------------------------------------------------------------------------
//          workspace presets
// ------------------------------------------------------------------------------------

void select_task(const std::string &name); // below (sidebar)

nlohmann::json preset_to_json(const preset_t &p) {
   return {{"name", p.name}, {"task", p.task}, {"description", p.description}, {"symmetry", p.symmetry},
           {"symm_radius", p.symm_radius}, {"map_radius", p.map_radius}, {"font_size", p.font_size},
           {"panels", p.panels}, {"coot_action", p.coot_action}, {"problems", p.problems}};
}

preset_t json_to_preset(const nlohmann::json &j) {
   preset_t p;
   p.name = j.value("name", ""); p.task = j.value("task", ""); p.description = j.value("description", "");
   p.symmetry = j.value("symmetry", false); p.symm_radius = j.value("symm_radius", 0.0);
   p.map_radius = j.value("map_radius", 0.0); p.font_size = j.value("font_size", 0);
   p.panels = j.value("panels", -1); p.coot_action = j.value("coot_action", ""); p.problems = j.value("problems", false);
   return p;
}

std::vector<preset_t> builtin_presets() {
   std::vector<preset_t> v;
   preset_t b; b.name = "Model building"; b.task = "Build"; b.map_radius = 15; b.symmetry = false; b.panels = 1;
   b.coot_action = "scene_preset_model_building_action";
   b.description = "Build page, symmetry off, 15 Å map radius, thin slab";
   v.push_back(b);
   preset_t l; l.name = "Ligand fitting"; l.task = "Ligands"; l.map_radius = 12; l.symmetry = false; l.panels = 1;
   l.description = "Ligands page, symmetry off, 12 Å map radius";
   v.push_back(l);
   preset_t c; c.name = "Crystal contacts"; c.task = "Symmetry"; c.symmetry = true; c.symm_radius = 25; c.panels = 1;
   c.description = "Symmetry page, neighbours shown within 25 Å";
   v.push_back(c);
   preset_t q; q.name = "Validation"; q.task = "Validate"; q.problems = true; q.panels = 1;
   q.description = "Validate page, problems marked on the model";
   v.push_back(q);
   preset_t f; f.name = "Figure making"; f.task = ""; f.panels = 0; f.font_size = 3; f.symmetry = false;
   f.coot_action = "scene_preset_figure_making_action";
   f.description = "Panels hidden, deeper slab, larger labels (bring the panels back with the header buttons)";
   v.push_back(f);
   return v;
}

std::vector<preset_t> user_presets() {
   std::vector<preset_t> v;
   nlohmann::json j = read_json_file(config_path("presets.json"), nlohmann::json::array());
   for (const auto &x : j) v.push_back(json_to_preset(x));
   return v;
}

void apply_preset(const preset_t &p) {
   if (!p.task.empty()) select_task(p.task);
   if (p.symmetry != symmetry_is_on()) set_symmetry_view(p.symmetry);
   if (p.symmetry && p.symm_radius > 0) set_symmetry_size(p.symm_radius);
   if (p.map_radius > 0) set_map_radius(p.map_radius);
   if (p.font_size > 0) set_font_size(p.font_size);
   if (p.panels >= 0) {
      if (sidebar_panel) gtk_widget_set_visible(sidebar_panel, p.panels == 1);
      if (inspector_panel) gtk_widget_set_visible(inspector_panel, p.panels == 1);
   }
   if (!p.coot_action.empty()) g_action_group_activate_action(G_ACTION_GROUP(coot_plus_app), p.coot_action.c_str(), nullptr);
   if (p.problems) show_problems_on_model(true);
   graphics_draw();
   graphics_info_t::add_status_bar_text("Workspace: " + p.name);
}

std::string current_task_name();

void save_current_as_preset(const std::string &name) {
   preset_t p;
   p.name = name;
   p.task = current_task_name();
   p.symmetry = symmetry_is_on();
   p.symm_radius = graphics_info_t::symmetry_search_radius;
   p.map_radius = get_map_radius();
   p.font_size = get_font_size();
   p.panels = (sidebar_panel && gtk_widget_get_visible(sidebar_panel)) ? 1 : 0;
   p.description = (p.task.empty() ? std::string("") : p.task + " page, ") + (p.symmetry ? "symmetry on" : "symmetry off") +
                   ", " + format_float(p.map_radius, 0) + " Å map radius";
   nlohmann::json j = read_json_file(config_path("presets.json"), nlohmann::json::array());
   nlohmann::json out = nlohmann::json::array();
   for (const auto &x : j) if (x.value("name", "") != name) out.push_back(x);
   out.push_back(preset_to_json(p));
   write_json_file(config_path("presets.json"), out);
   graphics_info_t::add_status_bar_text("Saved workspace preset “" + name + "”");
}

void delete_user_preset(const std::string &name) {
   nlohmann::json j = read_json_file(config_path("presets.json"), nlohmann::json::array()), out = nlohmann::json::array();
   for (const auto &x : j) if (x.value("name", "") != name) out.push_back(x);
   write_json_file(config_path("presets.json"), out);
}

GtkWidget *make_presets_menu_button() {
   GtkWidget *mb = gtk_menu_button_new();
   GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
   gtk_box_append(GTK_BOX(content), gtk_label_new("Workspace"));
   GtkWidget *arrow = gtk_label_new("▾");
   gtk_widget_add_css_class(arrow, "coot-plus-menu-arrow");
   gtk_box_append(GTK_BOX(content), arrow);
   gtk_menu_button_set_child(GTK_MENU_BUTTON(mb), content);
   gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(mb), FALSE);
   gtk_widget_set_focusable(mb, FALSE);
   gtk_widget_set_valign(mb, GTK_ALIGN_CENTER);
   gtk_widget_set_tooltip_text(mb, "Workspace presets: set up the window for a job in one click");
   GtkWidget *pop = gtk_popover_new();
   gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), pop);
   // rebuild the list each time it opens (user presets may have changed)
   g_signal_connect(pop, "show", G_CALLBACK(+[] (GtkWidget *pop, gpointer) {
                       GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
                       add_menu_heading(box, "Workspaces");
                       for (const auto &p : builtin_presets())
                          add_fn_entry(box, pop, p.name, p.description.c_str(), [p] () { apply_preset(p); });
                       std::vector<preset_t> up = user_presets();
                       if (!up.empty()) {
                          add_menu_heading(box, "Yours");
                          for (const auto &p : up)
                             add_fn_entry(box, pop, p.name, p.description.c_str(), [p] () { apply_preset(p); });
                       }
                       gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
                       add_fn_entry(box, pop, "Save current setup as a workspace…",
                                    "Remembers the task page, symmetry, map radius, label size and panels", [] () {
                          ask_name("Save Workspace", "Name for this setup:", "My setup",
                                   [] (const std::string &n) { save_current_as_preset(n); }); });
                       for (const auto &p : up) {
                          std::string n = p.name;
                          add_fn_entry(box, pop, "Delete “" + n + "”", nullptr, [n] () { delete_user_preset(n); });
                       }
                       gtk_popover_set_child(GTK_POPOVER(pop), box);
                    }), nullptr);
   return mb;
}

// ------------------------------------------------------------------------------------
//          customisable Common lists
// ------------------------------------------------------------------------------------


std::vector<std::string> custom_common(const std::string &task) {
   std::vector<std::string> v;
   GKeyFile *kf = g_key_file_new();
   if (g_key_file_load_from_file(kf, coot_plus_settings_file().c_str(), G_KEY_FILE_NONE, nullptr)) {
      gsize n = 0;
      std::string key = "common-" + task;
      gchar **l = g_key_file_get_string_list(kf, "interface", key.c_str(), &n, nullptr);
      for (gsize i=0; l && i<n; i++) v.push_back(l[i]);
      g_strfreev(l);
   }
   g_key_file_free(kf);
   return v;
}

void save_custom_common(const std::string &task, const std::vector<std::string> &keys, bool remove) {
   std::string fn = coot_plus_settings_file();
   GKeyFile *kf = g_key_file_new();
   g_key_file_load_from_file(kf, fn.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
   std::string key = "common-" + task;
   if (remove) g_key_file_remove_key(kf, "interface", key.c_str(), nullptr);
   else {
      std::vector<const char *> v;
      for (const auto &k : keys) v.push_back(k.c_str());
      g_key_file_set_string_list(kf, "interface", key.c_str(), v.data(), v.size());
   }
   gchar *dir = g_path_get_dirname(fn.c_str()); g_mkdir_with_parents(dir, 0755); g_free(dir);
   g_key_file_save_to_file(kf, fn.c_str(), nullptr);
   g_key_file_free(kf);
}



// ------------------------------------------------------------------------------------
//          console: Script and Snippets tabs
// ------------------------------------------------------------------------------------

GtkWidget *script_view = nullptr;
GtkTextBuffer *script_out = nullptr;
GtkWidget *script_out_view = nullptr;
GtkWidget *snippets_box = nullptr;
std::string script_file;
void refresh_snippets();

GtkTextBuffer *make_output_buffer(GtkWidget *view) {
   GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
   gtk_text_buffer_create_tag(b, "in",   "foreground", "#8ab4f8", nullptr);
   gtk_text_buffer_create_tag(b, "out",  nullptr);
   gtk_text_buffer_create_tag(b, "err",  "foreground", "#ff7b72", nullptr);
   gtk_text_buffer_create_tag(b, "hint", "foreground", "#e3b341", nullptr);
   gtk_text_buffer_create_tag(b, "info", "foreground", "#8b949e", nullptr);
   return b;
}

void print_to(GtkTextBuffer *b, GtkWidget *view, const std::string &text, const char *tag) {
   GtkTextIter end;
   gtk_text_buffer_get_end_iter(b, &end);
   gtk_text_buffer_insert_with_tags_by_name(b, &end, text.c_str(), -1, tag, nullptr);
   gtk_text_buffer_get_end_iter(b, &end);
   GtkTextMark *m = gtk_text_buffer_create_mark(b, nullptr, &end, FALSE);
   gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(view), m);
   gtk_text_buffer_delete_mark(b, m);
}

std::string script_text() {
   GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(script_view));
   GtkTextIter a, e;
   if (!gtk_text_buffer_get_selection_bounds(b, &a, &e)) gtk_text_buffer_get_bounds(b, &a, &e); // selection, else all
   char *t = gtk_text_buffer_get_text(b, &a, &e, FALSE);
   std::string r = t; g_free(t);
   return r;
}

void run_script_text() {
   std::string code = script_text();
   if (code.find_first_not_of(" \n\t") == std::string::npos) return;
   if (code.back() != '\n') code += "\n";
   print_to(script_out, script_out_view, "▶ Run" + std::string(script_file.empty() ? "" : " " + file_basename(script_file)) + "\n", "info");
   std::string out, err;
   // always run as a block (several statements)
   bool ok = coot_plus_run_python(code.find('\n') == code.size() - 1 ? code + "\n" : code, out, err);
   if (!out.empty()) print_to(script_out, script_out_view, out + (out.back() == '\n' ? "" : "\n"), "out");
   if (!ok) print_to(script_out, script_out_view, err + "\n", "err");
   else if (out.empty()) print_to(script_out, script_out_view, "(done)\n", "info");
   graphics_draw();
}

void set_script(const std::string &code) {
   gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(script_view)), code.c_str(), -1);
   if (console_notebook) gtk_notebook_set_current_page(GTK_NOTEBOOK(console_notebook), 1);
   gtk_widget_grab_focus(script_view);
}

void open_script_file() {
   GtkFileDialog *fd = gtk_file_dialog_new();
   gtk_file_dialog_set_title(fd, "Open Python Script");
   gtk_file_dialog_open(fd, GTK_WINDOW(console_window), nullptr, +[] (GObject *src, GAsyncResult *res, gpointer) {
                           GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, nullptr);
                           if (!f) return;
                           char *contents = nullptr; gsize len = 0;
                           if (g_file_load_contents(f, nullptr, &contents, &len, nullptr, nullptr)) {
                              char *p = g_file_get_path(f);
                              script_file = p ? p : "";
                              g_free(p);
                              set_script(std::string(contents, len));
                              g_free(contents);
                           }
                           g_object_unref(f);
                        }, nullptr);
   g_object_unref(fd);
}

void save_script_file(bool save_as) {
   std::string text;
   {
      GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(script_view));
      GtkTextIter a, e; gtk_text_buffer_get_bounds(b, &a, &e);
      char *t = gtk_text_buffer_get_text(b, &a, &e, FALSE); text = t; g_free(t);
   }
   if (!save_as && !script_file.empty()) {
      std::ofstream f(script_file); f << text;
      print_to(script_out, script_out_view, "Saved " + script_file + "\n", "info");
      return;
   }
   GtkFileDialog *fd = gtk_file_dialog_new();
   gtk_file_dialog_set_title(fd, "Save Python Script");
   gtk_file_dialog_set_initial_name(fd, script_file.empty() ? "coot-script.py" : file_basename(script_file).c_str());
   std::string *copy = new std::string(text);
   gtk_file_dialog_save(fd, GTK_WINDOW(console_window), nullptr, +[] (GObject *src, GAsyncResult *res, gpointer d) {
                           std::string *t = static_cast<std::string *>(d);
                           GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, nullptr);
                           if (f) {
                              char *p = g_file_get_path(f);
                              if (p) { script_file = p; std::ofstream o(p); o << *t;
                                       print_to(script_out, script_out_view, "Saved " + script_file + "\n", "info"); }
                              g_free(p);
                              g_object_unref(f);
                           }
                           delete t;
                        }, copy);
   g_object_unref(fd);
}

nlohmann::json load_user_snippets() { return read_json_file(config_path("snippets.json"), nlohmann::json::array()); }

GtkWidget *make_script_page() {
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8);
   gtk_widget_set_margin_top(box, 8);   gtk_widget_set_margin_bottom(box, 8);
   GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   GtkWidget *open = gtk_button_new_with_label("Open…");
   GtkWidget *save = gtk_button_new_with_label("Save");
   GtkWidget *save_as = gtk_button_new_with_label("Save As…");
   GtkWidget *as_snip = gtk_button_new_with_label("Save as Snippet…");
   GtkWidget *spacer = gtk_label_new("");
   gtk_widget_set_hexpand(spacer, TRUE);
   GtkWidget *run = gtk_button_new_with_label("Run  (⌘Return)");
   gtk_widget_add_css_class(run, "suggested-action");
   gtk_widget_set_tooltip_text(run, "Runs the selected lines, or the whole script if nothing is selected");
   for (GtkWidget *b : {open, save, save_as, as_snip}) gtk_box_append(GTK_BOX(bar), b);
   gtk_box_append(GTK_BOX(bar), spacer);
   gtk_box_append(GTK_BOX(bar), run);
   g_signal_connect(open, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { open_script_file(); }), nullptr);
   g_signal_connect(save, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { save_script_file(false); }), nullptr);
   g_signal_connect(save_as, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { save_script_file(true); }), nullptr);
   g_signal_connect(run, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { run_script_text(); }), nullptr);
   g_signal_connect(as_snip, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) {
                       ask_name("Save Snippet", "Name for this snippet:", "My snippet", [] (const std::string &n) {
                          GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(script_view));
                          GtkTextIter a, e; gtk_text_buffer_get_bounds(b, &a, &e);
                          char *t = gtk_text_buffer_get_text(b, &a, &e, FALSE);
                          nlohmann::json j = load_user_snippets();
                                  nlohmann::json out = nlohmann::json::array();
                          for (const auto &x : j) if (x.value("title", "") != n) out.push_back(x);
                          out.push_back({{"title", n}, {"description", "Your snippet"}, {"code", std::string(t)}});
                          g_free(t);
                          write_json_file(config_path("snippets.json"), out);
                          refresh_snippets();
                          print_to(script_out, script_out_view, "Saved snippet “" + n + "”\n", "info");
                       }); }), nullptr);

   script_view = gtk_text_view_new();
   gtk_text_view_set_monospace(GTK_TEXT_VIEW(script_view), TRUE);
   gtk_text_view_set_left_margin(GTK_TEXT_VIEW(script_view), 8);
   gtk_text_view_set_top_margin(GTK_TEXT_VIEW(script_view), 6);
   gtk_widget_add_css_class(script_view, "coot-plus-console-in");
   gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(script_view)),
                            "# A Coot script: coot, coot_utils and coot_gui are loaded.\n"
                            "# Pick one from Snippets, or write your own, then Run.\n\n", -1);
   GtkEventController *keys = gtk_event_controller_key_new();
   g_signal_connect(keys, "key-pressed", G_CALLBACK(+[] (GtkEventControllerKey *, guint keyval, guint, GdkModifierType st, gpointer) -> gboolean {
                       if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && (st & (GDK_META_MASK | GDK_CONTROL_MASK))) {
                          run_script_text(); return TRUE; }
                       if (keyval == GDK_KEY_Tab && !(st & GDK_SHIFT_MASK)) { // indent with 4 spaces
                          gtk_text_buffer_insert_at_cursor(gtk_text_view_get_buffer(GTK_TEXT_VIEW(script_view)), "    ", -1);
                          return TRUE; }
                       return FALSE; }), nullptr);
   gtk_widget_add_controller(script_view, keys);
   GtkWidget *sw = gtk_scrolled_window_new();
   gtk_widget_set_vexpand(sw, TRUE);
   gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), script_view);

   script_out_view = gtk_text_view_new();
   gtk_text_view_set_monospace(GTK_TEXT_VIEW(script_out_view), TRUE);
   gtk_text_view_set_editable(GTK_TEXT_VIEW(script_out_view), FALSE);
   gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(script_out_view), GTK_WRAP_WORD_CHAR);
   gtk_text_view_set_left_margin(GTK_TEXT_VIEW(script_out_view), 8);
   script_out = make_output_buffer(script_out_view);
   GtkWidget *osw = gtk_scrolled_window_new();
   gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(osw), 110);
   gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(osw), script_out_view);
   GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
   gtk_paned_set_start_child(GTK_PANED(paned), sw);
   gtk_paned_set_end_child(GTK_PANED(paned), osw);
   gtk_paned_set_position(GTK_PANED(paned), 300);
   gtk_widget_set_vexpand(paned, TRUE);
   gtk_box_append(GTK_BOX(box), bar);
   gtk_box_append(GTK_BOX(box), paned);
   return box;
}

void refresh_snippets() {
   if (!snippets_box) return;
   GtkWidget *c;
   while ((c = gtk_widget_get_first_child(snippets_box)) != nullptr) gtk_box_remove(GTK_BOX(snippets_box), c);
   auto add_row = [] (const std::string &title, const std::string &desc, const std::string &code, bool user) {
      GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_add_css_class(row, "coot-plus-molecule-row");
      GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_set_hexpand(texts, TRUE);
      GtkWidget *t = gtk_label_new(title.c_str());
      GtkWidget *d = gtk_label_new(desc.c_str());
      gtk_label_set_xalign(GTK_LABEL(t), 0.0);
      gtk_label_set_xalign(GTK_LABEL(d), 0.0);
      gtk_label_set_wrap(GTK_LABEL(d), TRUE);
      gtk_widget_add_css_class(t, "coot-plus-map-name");
      gtk_widget_add_css_class(d, "coot-plus-dim");
      gtk_box_append(GTK_BOX(texts), t);
      gtk_box_append(GTK_BOX(texts), d);
      gtk_widget_set_tooltip_text(texts, code.c_str());
      GtkWidget *edit = gtk_button_new_with_label("Open in Script");
      GtkWidget *run = gtk_button_new_with_label("Run");
      gtk_widget_set_valign(edit, GTK_ALIGN_CENTER);
      gtk_widget_set_valign(run, GTK_ALIGN_CENTER);
      g_signal_connect_data(edit, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer d) {
                               script_file.clear(); set_script(*static_cast<std::string *>(d)); }),
                            new std::string(code), +[] (gpointer d, GClosure *) { delete static_cast<std::string *>(d); }, GConnectFlags(0));
      g_signal_connect_data(run, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer d) {
                               script_file.clear(); set_script(*static_cast<std::string *>(d)); run_script_text(); }),
                            new std::string(code), +[] (gpointer d, GClosure *) { delete static_cast<std::string *>(d); }, GConnectFlags(0));
      gtk_box_append(GTK_BOX(row), texts);
      gtk_box_append(GTK_BOX(row), edit);
      gtk_box_append(GTK_BOX(row), run);
      if (user) {
         GtkWidget *del = gtk_button_new_with_label("✕");
         gtk_widget_add_css_class(del, "flat");
         gtk_widget_set_valign(del, GTK_ALIGN_CENTER);
         gtk_widget_set_tooltip_text(del, "Delete this snippet");
         g_signal_connect_data(del, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer d) {
                                  std::string n = *static_cast<std::string *>(d);
                                  nlohmann::json j = load_user_snippets();
                                  nlohmann::json out = nlohmann::json::array();
                                  for (const auto &x : j) if (x.value("title", "") != n) out.push_back(x);
                                  write_json_file(config_path("snippets.json"), out);
                                  g_idle_add(+[] (gpointer) -> gboolean { refresh_snippets(); return G_SOURCE_REMOVE; }, nullptr); }),
                               new std::string(title), +[] (gpointer d, GClosure *) { delete static_cast<std::string *>(d); }, GConnectFlags(0));
         gtk_box_append(GTK_BOX(row), del);
      }
      gtk_box_append(GTK_BOX(snippets_box), row);
   };
   nlohmann::json user = load_user_snippets();
   if (!user.empty()) {
      gtk_box_append(GTK_BOX(snippets_box), section_label("Yours"));
      for (const auto &x : user) add_row(x.value("title", ""), x.value("description", ""), x.value("code", ""), true);
   }
   gtk_box_append(GTK_BOX(snippets_box), section_label("Ready-made"));
   nlohmann::json built = py_json("__cp_plus.snippets()");
   if (built.is_array())
      for (const auto &x : built) add_row(x.value("title", ""), x.value("description", ""), x.value("code", ""), false);
}

GtkWidget *make_snippets_page() {
   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start(box, 10); gtk_widget_set_margin_end(box, 10);
   gtk_widget_set_margin_top(box, 8);    gtk_widget_set_margin_bottom(box, 8);
   GtkWidget *intro = gtk_label_new("Small scripts you can run as they are, or open in the Script tab to change. "
                                    "Most act on the model at the centre of the screen. Save your own from the Script tab.");
   gtk_label_set_wrap(GTK_LABEL(intro), TRUE);
   gtk_label_set_xalign(GTK_LABEL(intro), 0.0);
   gtk_widget_add_css_class(intro, "coot-plus-dim");
   snippets_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
   GtkWidget *sw = gtk_scrolled_window_new();
   gtk_widget_set_vexpand(sw, TRUE);
   gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), snippets_box);
   gtk_box_append(GTK_BOX(box), intro);
   gtk_box_append(GTK_BOX(box), sw);
   refresh_snippets();
   return box;
}

GtkWidget *make_card(GtkWidget *parent, const std::string &title) {
   GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_add_css_class(card, "coot-plus-card");
   gtk_box_append(GTK_BOX(card), section_label(title));
   gtk_box_append(GTK_BOX(parent), card);
   return card;
}

GtkWidget *small_label(const std::string &text) {
   GtkWidget *l = gtk_label_new(text.c_str());
   gtk_label_set_xalign(GTK_LABEL(l), 0.0);
   return l;
}

GtkWidget *make_inspector() {

   GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
   gtk_widget_add_css_class(box, "coot-plus-inspector");

   // --- Molecules
   GtkWidget *card = make_card(box, "Molecules");
   molecules_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
   gtk_box_append(GTK_BOX(card), molecules_box);

   // --- Selected
   card = make_card(box, "Selected");
   sel_title_label = gtk_label_new("Nothing selected");
   gtk_label_set_xalign(GTK_LABEL(sel_title_label), 0.0);
   gtk_widget_add_css_class(sel_title_label, "coot-plus-selected-title");
   sel_detail_label = gtk_label_new("");
   gtk_label_set_xalign(GTK_LABEL(sel_detail_label), 0.0);
   gtk_label_set_wrap(GTK_LABEL(sel_detail_label), TRUE);
   gtk_label_set_max_width_chars(GTK_LABEL(sel_detail_label), 30);
   gtk_widget_add_css_class(sel_detail_label, "coot-plus-dim");
   gtk_box_append(GTK_BOX(card), sel_title_label);
   gtk_box_append(GTK_BOX(card), sel_detail_label);
   {
      GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
      const std::vector<std::pair<std::string, std::string> > selected_tools = {
         {"app.residue_info_action", "Info"}, {"kb:Label/Unlabel Active Atom", "Label"}, {"kb:Sphere Refine", "Refine"}};
      for (const auto &t : selected_tools) {
         const command_t *c = resolve_tool(t.first);
         if (!c) continue;
         GtkWidget *b = gtk_button_new_with_label(t.second.c_str());
         gtk_widget_add_css_class(b, "coot-plus-pill");
         gtk_widget_set_focusable(b, FALSE);
         gtk_widget_set_tooltip_text(b, tooltip_for(*c).c_str());
         command_t *copy = new command_t(*c);
         g_signal_connect_data(b, "clicked",
                               G_CALLBACK(+[] (GtkButton *, gpointer data) { run_command(*static_cast<command_t *>(data)); }),
                               copy, +[] (gpointer data, GClosure *) { delete static_cast<command_t *>(data); },
                               GConnectFlags(0));
         gtk_box_append(GTK_BOX(row), b);
      }
      gtk_box_append(GTK_BOX(card), row);
      gtk_box_append(GTK_BOX(card), make_stepping_row());
   }

   // --- Ligand (only when centred on one)
   make_ligand_card(box);

   // --- Symmetry
   card = make_card(box, "Symmetry");
   gtk_box_append(GTK_BOX(card), make_symmetry_switch_row(&symmetry_switch_inspector));
   symm_radius_label = small_label("Radius");
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
   gtk_box_append(GTK_BOX(card), symm_radius_label);
   gtk_box_append(GTK_BOX(card), symm_radius_scale);
   unit_cell_check = gtk_check_button_new_with_label("Show unit cell");
   gtk_widget_set_focusable(unit_cell_check, FALSE);
   gtk_widget_set_tooltip_text(unit_cell_check, "Draw the unit cell box of every model");
   g_signal_connect(unit_cell_check, "toggled",
                    G_CALLBACK(+[] (GtkCheckButton *b, gpointer) {
                       if (updating_widgets) return;
                       set_show_unit_cells_all(gtk_check_button_get_active(b));
                    }), nullptr);
   gtk_box_append(GTK_BOX(card), unit_cell_check);

   // --- Labels
   card = make_card(box, "Labels");
   {
      GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      label_size_label = small_label("Label size");
      gtk_widget_set_hexpand(label_size_label, TRUE);
      GtkWidget *steps = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_add_css_class(steps, "linked");
      GtkWidget *smaller = gtk_button_new_with_label("A−");
      GtkWidget *bigger  = gtk_button_new_with_label("A+");
      for (GtkWidget *b : {smaller, bigger}) {
         gtk_widget_set_focusable(b, FALSE);
         gtk_box_append(GTK_BOX(steps), b);
      }
      gtk_widget_set_tooltip_text(smaller, "Smaller atom labels");
      gtk_widget_set_tooltip_text(bigger,  "Bigger atom labels");
      auto step = +[] (GtkButton *, gpointer d) { step_label_size(GPOINTER_TO_INT(d)); };
      g_signal_connect(smaller, "clicked", G_CALLBACK(step), GINT_TO_POINTER(-1));
      g_signal_connect(bigger,  "clicked", G_CALLBACK(step), GINT_TO_POINTER(+1));
      gtk_box_append(GTK_BOX(row), label_size_label);
      gtk_box_append(GTK_BOX(row), steps);
      gtk_box_append(GTK_BOX(card), row);
      const command_t *c = resolve_tool("app.clear_atom_labels_action");
      if (c) gtk_box_append(GTK_BOX(card), make_tool_button(*c));
   }

   GtkWidget *scrolled = gtk_scrolled_window_new();
   gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), box);
   gtk_widget_set_size_request(scrolled, 300, -1);
   gtk_widget_add_css_class(scrolled, "coot-plus-inspector-scroll");

   inspector = scrolled;
   g_timeout_add(500, +[] (gpointer) -> gboolean { track_view_changes(); return G_SOURCE_CONTINUE; }, nullptr);
   update_molecules_section();
   g_timeout_add(400, inspector_tick, nullptr);
   return scrolled;
}

// ------------------------------------------------------------------------------------
//                                  header
// ------------------------------------------------------------------------------------

GtkWidget *icon_button(const char *icon, const char *tooltip) {
   GtkWidget *b = gtk_button_new_from_icon_name(icon);
   gtk_widget_add_css_class(b, "flat");
   gtk_widget_add_css_class(b, "coot-plus-icon-button");
   gtk_widget_set_focusable(b, FALSE);
   gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
   gtk_widget_set_tooltip_text(b, tooltip);
   return b;
}

GtkWidget *make_panel_toggle(const char *icon, const char *tooltip, GtkWidget *panel, const char *setting) {
   GtkWidget *b = gtk_toggle_button_new();
   gtk_button_set_icon_name(GTK_BUTTON(b), icon);
   gtk_widget_add_css_class(b, "flat");
   gtk_widget_add_css_class(b, "coot-plus-icon-button");
   gtk_widget_set_focusable(b, FALSE);
   gtk_widget_set_tooltip_text(b, tooltip);
   gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
   bool shown = load_coot_plus_setting_bool(setting, true);
   gtk_widget_set_visible(panel, shown);
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), shown);
   g_object_set_data(G_OBJECT(b), "setting", const_cast<char *>(setting));
   g_signal_connect(b, "toggled",
                    G_CALLBACK(+[] (GtkToggleButton *t, gpointer p) {
                       bool on = gtk_toggle_button_get_active(t);
                       gtk_widget_set_visible(GTK_WIDGET(p), on);
                       save_coot_plus_setting_bool(static_cast<const char *>(g_object_get_data(G_OBJECT(t), "setting")), on);
                    }), panel);
   // keep the button in step when the panel is shown/hidden another way (search commands)
   g_signal_connect(panel, "notify::visible",
                    G_CALLBACK(+[] (GObject *o, GParamSpec *, gpointer btn) {
                       bool on = gtk_widget_get_visible(GTK_WIDGET(o));
                       if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn)) != on)
                          gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), on);
                    }), b);
   return b;
}

void set_classic_toolbar_visible(bool state) {
   if (!classic_toolbar) return;
   gtk_widget_set_visible(classic_toolbar, state);
   save_coot_plus_setting_bool("classic-toolbar", state);
   graphics_info_t::add_status_bar_text(state ? "Classic build toolbar shown (hide it again from the search: \"classic\")"
                                              : "Classic build toolbar hidden - its tools are in the Build task");
}

void apply_toolbar_tooltips() {
   for (const auto &d : descriptions()) {
      if (d.first.rfind("tb:", 0) != 0) continue;
      GtkWidget *w = widget_from_builder(d.first.substr(3));
      if (w) gtk_widget_set_tooltip_text(w, d.second.c_str());
   }
}

void load_coot_plus_css() {
   const char *css =
      // header
      ".coot-plus-header { padding: 5px 8px; border-bottom: 1px solid alpha(currentColor, 0.10);"
      "  background-color: alpha(currentColor, 0.035); }\n"
      ".coot-plus-app-name { font-weight: 700; margin: 0 10px 0 4px; }\n"
      ".coot-plus-search { min-height: 30px; border-radius: 8px; min-width: 360px; }\n"
      ".coot-plus-icon-button { min-width: 28px; min-height: 28px; padding: 2px 6px; border-radius: 7px; }\n"
      ".coot-plus-header .linked button { padding: 2px 10px; min-height: 26px; }\n"
      ".coot-plus-header-sep { margin: 6px 6px; opacity: 0.4; }\n"
      ".coot-plus-header checkbutton, .coot-plus-header button { margin: 0 1px; }\n"
      // sidebar
      ".coot-plus-sidebar { background-color: alpha(currentColor, 0.03); border-right: 1px solid alpha(currentColor, 0.10); }\n"
      ".coot-plus-task-list { background: transparent; padding: 8px 6px 6px 6px; }\n"
      ".coot-plus-task-list row { padding: 6px 10px; border-radius: 7px; margin: 1px 0; }\n"
      ".coot-plus-task-list row:hover { background-color: alpha(currentColor, 0.07); }\n"

      ".coot-plus-sidebar-sep { margin: 2px 12px; opacity: 0.5; }\n"
      ".coot-plus-task-page { padding: 6px 6px 16px 6px; }\n"
      ".coot-plus-page-title { font-weight: 700; font-size: 1.15em; margin: 4px 8px 0 8px; }\n"
      ".coot-plus-page-hint { opacity: 0.6; font-size: 0.9em; margin: 0 8px 4px 8px; }\n"
      ".coot-plus-section { font-size: 0.75em; font-weight: 700; letter-spacing: 1px; opacity: 0.5; margin: 12px 8px 3px 8px; }\n"
      ".coot-plus-tool { padding: 4px 8px; border-radius: 6px; min-height: 24px; }\n"
      ".coot-plus-tool:hover { background-color: alpha(currentColor, 0.08); }\n"
      "menubutton.coot-plus-tool { padding: 0; min-height: 0; }\n"
      "menubutton.coot-plus-tool > button { padding: 4px 8px; border-radius: 6px; min-height: 24px; }\n"
      ".coot-plus-menu-arrow { opacity: 0.5; font-size: 0.8em; }\n"
      ".coot-plus-tool-icon { opacity: 0.9; }\n"
      ".coot-plus-shortcut { font-family: monospace; font-size: 0.8em; opacity: 0.55; padding: 0 5px;"
      "  border: 1px solid alpha(currentColor, 0.25); border-radius: 4px; }\n"
      ".coot-plus-link { font-size: 0.85em; opacity: 0.7; margin: 12px 4px 0 4px; }\n"
      ".coot-plus-switch-row { padding: 4px 8px; }\n"
      // inspector
      ".coot-plus-inspector-scroll { border-left: 1px solid alpha(currentColor, 0.10); background-color: alpha(currentColor, 0.03); }\n"
      ".coot-plus-inspector { padding: 10px; }\n"
      ".coot-plus-card { background-color: alpha(currentColor, 0.05); border-radius: 10px; padding: 10px 12px 12px 12px; }\n"
      ".coot-plus-card .coot-plus-section { margin: 0 0 2px 0; }\n"
      ".coot-plus-selected-title { font-weight: 700; font-size: 1.2em; }\n"
      ".coot-plus-dim { opacity: 0.65; font-size: 0.9em; }\n"
      ".coot-plus-pill { border-radius: 7px; padding: 3px 6px; }\n"
      ".coot-plus-map-name { font-weight: 600; }\n"
      ".coot-plus-readout { font-size: 0.85em; opacity: 0.7; font-feature-settings: \"tnum\"; }\n"
      ".coot-plus-drag-number { font-feature-settings: \"tnum\"; padding: 1px 7px; border-radius: 5px; "
      "background: alpha(currentColor, 0.08); }\n"
      ".coot-plus-drag-number:hover { background: alpha(currentColor, 0.16); }\n"
      ".coot-plus-drag-entry { min-height: 0; padding: 0 4px; }\n"
      ".coot-plus-molecule-row { padding: 3px 0; }\n"
      ".coot-plus-warning { color: #f0b429; font-size: 0.85em; }\n"
      ".coot-plus-close { min-width: 24px; min-height: 24px; padding: 0 6px; opacity: 0.8; border-radius: 6px; font-size: 1.05em; }\n"
      ".coot-plus-molecule-row menubutton > button { min-width: 24px; min-height: 24px; padding: 0 4px; }\n"
      ".coot-plus-close:hover { opacity: 1; background-color: alpha(#e5534b, 0.25); }\n"
      ".coot-plus-menu-entry { padding: 5px 12px; }\n"
      ".coot-plus-dm-button { padding: 3px 10px; border-radius: 7px; font-weight: 600;"
      "  margin-right: 6px; }\n"
      ".coot-plus-help { border-top: 1px solid alpha(currentColor, 0.10); padding: 8px 12px 10px 12px;"
      "  background-color: alpha(currentColor, 0.04); }\n"
      ".coot-plus-help-title { font-weight: 700; font-size: 0.9em; }\n"
      ".coot-plus-help-text { font-size: 0.85em; opacity: 0.8; }\n"
      ".coot-plus-expander { margin: 10px 4px 0 4px; }\n"
      ".coot-plus-expander > title { padding: 4px 4px; font-weight: 600; opacity: 0.85; }\n"
      ".coot-plus-mode-banner { background-color: alpha(#1f2328, 0.92); color: #ffffff; border-radius: 10px;"
      "  padding: 6px 8px 6px 14px; box-shadow: 0 2px 10px alpha(black, 0.4); font-weight: 600; }\n"
      ".coot-plus-mode-dot { color: #4cc38a; }\n"
      ".coot-plus-mode-cancel { padding: 2px 10px; border-radius: 7px; font-weight: 400; }\n"
      ".coot-plus-message { padding: 6px 12px; border-top: 1px solid alpha(currentColor, 0.12); }\n"
      ".coot-plus-msg-error   { background-color: alpha(#e5534b, 0.22); }\n"
      ".coot-plus-msg-warning { background-color: alpha(#f0b429, 0.20); }\n"

      ".coot-plus-msg-icon { font-size: 1.1em; }\n"
      ".coot-plus-star { color: #f0b429; }\n"
      ".coot-plus-swatch { min-width: 0; min-height: 0; padding: 0; }\n"
      ".coot-plus-swatch > button { min-width: 26px; min-height: 12px; padding: 1px; border-radius: 4px; }\n"
      ".coot-plus-mono { font-family: monospace; }\n"
      ".coot-plus-console-out, .coot-plus-console-in { font-size: 0.95em; }\n"
      ".coot-plus-console-in { border: 1px solid alpha(currentColor, 0.25); border-radius: 6px; }\n"
      ".coot-plus-console-doc { font-family: monospace; font-size: 0.85em; opacity: 0.8; padding: 2px 4px; }\n"
      ".coot-plus-type-tag { font-size: 0.62em; font-weight: 800; letter-spacing: 0.5px; padding: 0 3px; border-radius: 3px; }\n"
      ".coot-plus-type-model { color: #8ab4f8; background-color: alpha(#8ab4f8, 0.15); }\n"
      ".coot-plus-type-map   { color: #7ee2a8; background-color: alpha(#7ee2a8, 0.15); }\n"
      ".coot-plus-type-diff  { color: #f5a97f; background-color: alpha(#f5a97f, 0.15); }\n"
      ".coot-plus-seq-strip { min-height: 26px; }\n"
      ".coot-plus-seq-cell { font-family: monospace; font-size: 0.9em; padding: 2px 2px; border-radius: 3px; }\n"
      ".coot-plus-seq-cell:hover { background-color: alpha(currentColor, 0.15); }\n"
      ".coot-plus-seq-het { opacity: 0.6; }\n"
      ".coot-plus-seq-current { font-weight: 700; }\n"
      ".coot-plus-ligand-card { border: 1px solid alpha(#e3b341, 0.35); }\n"
      ".coot-plus-serious { color: #ff7b72; }\n"
      ".coot-plus-minor { color: #e3b341; }\n"
      // search results
      ".coot-plus-results row { border-radius: 6px; }\n";
   GtkCssProvider *provider = gtk_css_provider_new();
   gtk_css_provider_load_from_string(provider, css);
   gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

// icons for menu actions (Coot's own pixmaps)
const std::map<std::string, std::string> &action_icons() {
   static const std::map<std::string, std::string> m = {
      {"app.auto_open_mtz_action", "map"},
      {"app.open_map_action", "map"}, {"app.open_dataset_action", "map"},
      {"app.show_accession_code_fetch_frame_oca", "connect-to-world"},
      {"app.show_accession_code_fetch_frame_eds", "connect-to-world"},
      {"app.show_accession_code_fetch_frame_pdb_redo", "connect-to-world"},
      {"app.show_accession_code_fetch_frame_uniprot_id", "connect-to-world"},
      {"app.fetch_pdbe_ligand_description_action", "connect-to-world"},
      {"app.fetch_ligand_restraints_from_github_action", "connect-to-world"},
      {"app.get_monomer_action", "go-to-ligand"}, {"app.go_to_atom_action", "go-to-atom"},
      {"app.sequence_view_action", "sequence-view"}, {"app.map_parameters_action", "map"},
      {"app.make_a_difference_map_action", "diff-map"}, {"app.draw_cell_and_symmetry_action", "cell+symm"},
      {"app.undo_symmetry_view_action", "cell+symm"}, {"app.save_symmetry_coordinates_action", "cell+symm"},
      {"app.refine_sphere", "refine-1"}, {"app.refine_range", "refine-1"}, {"app.refine_chain", "refine-1"},
      {"app.refine_regularize_sphere", "regularize-1"}, {"app.fill_partial_residues_action", "auto-fit-rotamer"},
      {"app.rigid_body_fit_residue_range_action", "rigid-body"}, {"app.rigid_body_fit_chain_action", "rigid-body"},
      {"app.rotate_translate_residue_range", "rtz"}, {"app.rotate_translate_chain", "rtz"},
      {"app.delete_item_residue", "delete"}, {"app.delete_item_side_chain", "delete"},
      {"app.delete_item_atom", "delete"}, {"app.delete_item_water", "delete"},
      {"app.delete_item_residue_range", "delete"}, {"app.find_ligands_action", "go-to-ligand"},
      {"app.unmodelled_blobs_action", "unknown-blob"}, {"app.ligand_builder_action", "layla_6arom"},
      {"app.SMILES_to_3D_action", "smiles"}, {"app.show_validation_graphs_dialog", "coot-plus-tool-graphs-symbolic"},
      {"app.ramachandran_plot_action", "coot-plus-tool-rama-plot-symbolic"},
      {"app.open_coordinates_action", "coot-plus-load-symbolic"},
      {"app.save_coordinates_action", "coot-plus-save-symbolic"},
      {"app.save_state_action", "coot-plus-tool-save-state-symbolic"},
      {"app.screenshot_action", "coot-plus-tool-screenshot-symbolic"},
      {"app.jiggle_fit_active_residue_action", "coot-plus-tool-jiggle-symbolic"},
      {"app.geometric_distortions_for_ligand_action", "coot-plus-tool-distortions-symbolic"},
      {"app.import_cif_dictionary_action", "coot-plus-tool-import-dictionary-symbolic"},
      {"app.validation_outliers_action", "coot-plus-tool-outliers-symbolic"},
      {"app.difference_map_peaks_action", "diff-map"}, {"app.atoms_overlaps_action", "probe-clash"},
      {"app.all_atom_contact_dots_molprobity_action", "probe-clash"}, {"app.check_delete_waters_action", "water-drop"},
      {"app.residue_info_action", "residue-info"}, {"app.add_an_atom_action", "atom-at-pointer"},
      {"app.environment_distances_action", "env-distances"}, {"app.pointer_distances_action", "pointer-distances"}};
   return m;
}

// What each control does - written from the source, only where its behaviour was checked.
// key: "app.action", "tb:builder-id", "kb:Key binding description", "menu:builder-menu-id"
const std::map<std::string, std::string> &descriptions() {
   static const std::map<std::string, std::string> m = {
#include "coot-plus-descriptions.inc"
   };
   return m;
}

} // namespace


void coot_plus_message(const std::string &kind, const std::string &text, const std::string &hint) {
   show_message(kind, text, hint);
}

void coot_plus_log_command(const std::string &line) {
   log_line(line);
}

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
   apply_accent(load_coot_plus_setting_string("accent", "#3584e4"));
   apply_theme(load_coot_plus_setting_string("theme", "dark"));

   GtkWidget *main_hbox     = widget_from_builder("main_window_hbox");
   GtkWidget *toolbar_outer = widget_from_builder("main_window_toolbar_hbox_outer");
   GtkWidget *toolbar_left  = widget_from_builder("main_window_toolbar_hbox");
   GtkWidget *toolbar_end   = widget_from_builder("main_window_toolbar_hbox_end");
   GtkWidget *graphics      = widget_from_builder("main_window_graphics_overlay");
   classic_toolbar          = widget_from_builder("main_window_vbox_inner");
   if (!main_hbox || !toolbar_outer || !toolbar_left || !graphics) {
      g_warning("coot-plus: main window widgets not found - task interface not added");
      return;
   }

   apply_toolbar_tooltips();
   compact_sidebar = load_coot_plus_setting_bool("compact-sidebar", false);
   {
      GKeyFile *kf = g_key_file_new();
      if (g_key_file_load_from_file(kf, coot_plus_settings_file().c_str(), G_KEY_FILE_NONE, nullptr)) {
         gchar *pf = g_key_file_get_string(kf, "interface", "phenix-folder", nullptr);
         if (pf) { phenix_folder = pf; g_free(pf); }
      }
      g_key_file_free(kf);
   }
   graphics_info_t::trackpad_drag_rotates = load_coot_plus_setting_bool("trackpad-drag-rotates", false);
   graphics_info_t::double_click_centres  = load_coot_plus_setting_bool("double-click-centres", true);
   if (GTK_IS_OVERLAY(graphics)) gtk_overlay_add_overlay(GTK_OVERLAY(graphics), make_mode_banner());
   if (GtkWidget *vbox = widget_from_builder("main_window_vbox")) {
      GtkWidget *statusbar = widget_from_builder("main_window_statusbar");
      GtkWidget *strip = make_message_strip();
      if (statusbar) gtk_box_insert_child_after(GTK_BOX(vbox), strip, gtk_widget_get_prev_sibling(statusbar));
      else gtk_box_append(GTK_BOX(vbox), strip);
   }
   add_preferences_page();
#ifndef HAVE_VTE
   // without VTE (this macOS build) the AI-terminal button does nothing - hide it
   if (GtkWidget *ai = widget_from_builder("claude_ai_button")) gtk_widget_set_visible(ai, FALSE);
#endif

   // --- panels
   GtkWidget *side = make_task_sidebar();
   sidebar_panel = side;
   // children with hexpand (labels) would otherwise make the panels share the 3D view's space
   gtk_widget_set_hexpand(side, FALSE);
   gtk_box_prepend(GTK_BOX(main_hbox), side);
   gtk_widget_set_hexpand(graphics, TRUE);
   GtkWidget *insp = make_inspector();
   inspector_panel = insp;
   gtk_widget_set_hexpand(insp, FALSE);
   gtk_box_insert_child_after(GTK_BOX(main_hbox), insp, graphics);
   if (classic_toolbar)
      gtk_widget_set_visible(classic_toolbar, load_coot_plus_setting_bool("classic-toolbar", false));

   // --- header: the old icon toolbar row is replaced; its buttons are in the Navigate task
   // the old icon row: hide its own buttons, but keep the row - Coot adds toggles to it at
   // run time (e.g. "Atom Overlaps" on/off) and those must stay reachable
   for (GtkWidget *w = gtk_widget_get_first_child(toolbar_left); w; w = gtk_widget_get_next_sibling(w))
      gtk_widget_set_visible(w, FALSE);
   gtk_widget_remove_css_class(toolbar_left, "toolbar");
   gtk_widget_add_css_class(toolbar_left, "coot-plus-runtime-toggles");
   gtk_widget_add_css_class(toolbar_outer, "coot-plus-header");
   if (toolbar_end) gtk_widget_remove_css_class(toolbar_end, "toolbar");
   if (GtkWidget *w = widget_from_builder("add_module_menubutton"))
      if (GTK_IS_MENU_BUTTON(w)) gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(w), FALSE);
   if (GtkWidget *w = widget_from_builder("python_scripting_button"))
      gtk_widget_add_css_class(w, "flat");
   GtkWidget *expander = widget_from_builder("toolbar_expander");
   if (expander) gtk_widget_set_visible(expander, FALSE);

   GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
   gtk_widget_set_valign(left, GTK_ALIGN_CENTER);
   gtk_box_append(GTK_BOX(left), make_panel_toggle("coot-plus-sidebar-symbolic", "Show/hide the task sidebar", side, "show-sidebar"));
   GtkWidget *name = gtk_label_new("Coot+");
   gtk_widget_add_css_class(name, "coot-plus-app-name");
   gtk_box_append(GTK_BOX(left), name);

   GtkWidget *center = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
   gtk_widget_set_hexpand(center, TRUE);
   gtk_widget_set_halign(center, GTK_ALIGN_CENTER);
   GtkWidget *entry = make_search_entry();
   gtk_widget_add_css_class(entry, "coot-plus-search");
   gtk_box_append(GTK_BOX(center), entry);

   GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
   gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
   graphics_info_t::scroll_wheel_zooms = load_coot_plus_setting_bool("scroll-wheel-zooms", false);
   gtk_box_append(GTK_BOX(right), make_presets_menu_button());
   gtk_box_append(GTK_BOX(right), make_scroll_mode_switch());
   gtk_box_append(GTK_BOX(right), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
   struct hb { const char *icon; const char *tip; const char *toolbar_id; };
   for (const hb &h : {hb{"coot-plus-undo-symbolic", "Undo Edit: undo the last change to the model's atoms (Ctrl-Z).\n"
                                                     "For display changes (dots, colours, contours...) use Undo View.", "undo_button"},
                       hb{"coot-plus-redo-symbolic", "Redo Edit (Ctrl-Y)", "redo_button"}}) {
      GtkWidget *b = icon_button(h.icon, h.tip);
      g_signal_connect(b, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer id) {
                          GtkWidget *w = widget_from_builder(static_cast<const char *>(id));
                          if (w) g_signal_emit_by_name(w, "clicked");
                       }), const_cast<char *>(h.toolbar_id));
      gtk_box_append(GTK_BOX(right), b);
   }
   {
      GtkWidget *uv = gtk_button_new();
      GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
      gtk_box_append(GTK_BOX(content), gtk_image_new_from_icon_name("coot-plus-undo-symbolic"));
      gtk_box_append(GTK_BOX(content), gtk_label_new("View"));
      gtk_button_set_child(GTK_BUTTON(uv), content);
      gtk_widget_add_css_class(uv, "flat");
      gtk_widget_add_css_class(uv, "coot-plus-icon-button");
      gtk_widget_set_focusable(uv, FALSE);
      gtk_widget_set_valign(uv, GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text(uv, "Undo View: undo the last display change - dots and overlaps, ribbons and surfaces, "
                                      "show/hide, contour levels, symmetry, drawing style, atom labels.\n"
                                      "It never changes the model (that's Undo Edit, to the left).");
      g_signal_connect(uv, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { undo_view_change(); }), nullptr);
      gtk_box_append(GTK_BOX(right), uv);
   }
   {
      GtkWidget *dm = gtk_button_new();
      GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      GtkWidget *img = gtk_image_new_from_icon_name("coot-plus-molecules-symbolic");
      gtk_box_append(GTK_BOX(content), img);
      gtk_box_append(GTK_BOX(content), gtk_label_new("Display Manager"));
      gtk_button_set_child(GTK_BUTTON(dm), content);
      gtk_widget_add_css_class(dm, "coot-plus-dm-button");
      gtk_widget_set_focusable(dm, FALSE);
      gtk_widget_set_valign(dm, GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text(dm, "Display Manager: show/hide, restyle and close molecules and maps (F8).\n"
                                      "The quick list is at the top of the inspector.");
      g_signal_connect(dm, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { open_display_manager(); }), nullptr);
      gtk_box_prepend(GTK_BOX(right), dm);
   }
   for (GtkWidget *w = gtk_widget_get_first_child(right); w; w = gtk_widget_get_next_sibling(w))
      if (GTK_IS_SEPARATOR(w)) gtk_widget_add_css_class(w, "coot-plus-header-sep");

   gtk_box_prepend(GTK_BOX(toolbar_outer), left);
   gtk_box_insert_child_after(GTK_BOX(toolbar_outer), center, left);
   g_object_ref(toolbar_left);
   gtk_box_remove(GTK_BOX(toolbar_outer), toolbar_left);
   gtk_box_insert_child_after(GTK_BOX(toolbar_outer), toolbar_left, center);
   g_object_unref(toolbar_left);
   gtk_box_insert_child_after(GTK_BOX(toolbar_outer), right, toolbar_left);
   {
      // the Python console replaces the one-line "Py" entry (still there for scripts)
      if (GtkWidget *py = widget_from_builder("python_scripting_button")) gtk_widget_set_visible(py, FALSE);
      GtkWidget *pb = gtk_button_new();
      GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
      GtkWidget *img = gtk_image_new_from_icon_name("coot-plus-tool-python-symbolic");
      gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
      gtk_box_append(GTK_BOX(content), img);
      gtk_box_append(GTK_BOX(content), gtk_label_new("Python"));
      gtk_button_set_child(GTK_BUTTON(pb), content);
      gtk_widget_add_css_class(pb, "flat");
      gtk_widget_add_css_class(pb, "coot-plus-icon-button");
      gtk_widget_set_focusable(pb, FALSE);
      gtk_widget_set_valign(pb, GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text(pb, "Python console: run coot commands, with history and Tab completion");
      g_signal_connect(pb, "clicked", G_CALLBACK(+[] (GtkButton *, gpointer) { show_python_console(); }), nullptr);
      gtk_box_append(GTK_BOX(toolbar_outer), pb);
   }
   gtk_box_append(GTK_BOX(toolbar_outer), make_more_menu());
   GtkWidget *insp_toggle = make_panel_toggle("coot-plus-inspector-symbolic", "Show/hide the inspector", insp, "show-inspector");
   gtk_box_append(GTK_BOX(toolbar_outer), insp_toggle);

   GtkWidget *window = graphics_info_t::get_main_window();
   if (window) {
      add_command_search_shortcut(window);
      add_escape_cancels_pick_modes(window);
   }

   // the menu item: About > Search Commands...
   GSimpleAction *a = g_simple_action_new("command_search_action", NULL);
   g_signal_connect(a, "activate", G_CALLBACK(+[] (GSimpleAction *, GVariant *, gpointer) {
                                                 coot_plus_show_command_search();
                                              }), nullptr);
   g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(a));

   // Calculate > Scripting > Python opens the console
   g_action_map_remove_action(G_ACTION_MAP(application), "scripting_python_action");
   GSimpleAction *pa = g_simple_action_new("scripting_python_action", NULL);
   g_signal_connect(pa, "activate", G_CALLBACK(+[] (GSimpleAction *, GVariant *, gpointer) { show_python_console(); }), nullptr);
   g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(pa));

   apply_compact_sidebar(compact_sidebar);
   refresh_recent_and_groups();

   // the command log starts now (what Coot+ itself queried while setting up is not the user's)
   history_seen = graphics_info_t::history_list.history_strings.size();
   g_timeout_add(500, watch_history, nullptr);
}
