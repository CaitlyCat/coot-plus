/*
 * src/coot-plus-ruler.cc
 *
 * Coot+ ruler - see coot-plus-ruler.hh. Drawn with Coot's own mesh and label code
 * (like Measures > Distance), but the ends are free points, not atoms.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or (at
 * your option) any later version.
 */

#include "graphics-info.h" // first: it includes Python.h
#include "coot-plus-ruler.hh"
#include "c-interface.h"
#include "coot-utils/cylinder.hh"
#include <glm/glm.hpp>
#include <cmath>

namespace {

bool      on = false;
bool      placed = false;
glm::vec3 end_a(0,0,0), end_b(0,0,0);
int       grabbed = -1;            // -1 none, 0 end A, 1 end B, 2 the whole ruler
glm::vec3 grab_offset_a, grab_offset_b;
float     grab_depth = 0;          // NDC depth of the grabbed point
bool      dirty = true;
Mesh      mesh("coot-plus-ruler");

const glm::vec4 ruler_colour(1.0f, 0.72f, 0.22f, 1.0f);   // amber: not an atom colour

glm::mat4 mvp() { return graphics_info_t::get_molecule_mvp(stereo_eye_t::MONO); }

// world -> (screen x, screen y, NDC depth)
glm::vec3 to_screen(const glm::vec3 &p) {
   GtkAllocation a = graphics_info_t::get_glarea_allocation();
   glm::vec4 c = mvp() * glm::vec4(p, 1.0f);
   if (c.w == 0) return glm::vec3(-1e6, -1e6, 0);
   glm::vec3 n = glm::vec3(c) / c.w;
   return glm::vec3((n.x + 1.0f) * 0.5f * a.width, (1.0f - n.y) * 0.5f * a.height, n.z);
}

// (screen x, screen y, NDC depth) -> world
glm::vec3 from_screen(double x, double y, float ndc_z) {
   GtkAllocation a = graphics_info_t::get_glarea_allocation();
   float nx = x / (a.width * 0.5f) - 1.0f;
   float ny = 1.0f - y / (a.height * 0.5f);
   glm::vec4 w = glm::inverse(mvp()) * glm::vec4(nx, ny, ndc_z, 1.0f);
   return glm::vec3(w) / w.w;
}

glm::vec3 rotation_centre() {
   coot::Cartesian rc = graphics_info_t::RotationCentre();
   return glm::vec3(rc.x(), rc.y(), rc.z());
}

// 10 A across the middle of the view, at the depth of the rotation centre
void place_at_centre() {
   glm::vec3 rc = rotation_centre();
   glm::vec3 s = to_screen(rc);
   glm::vec3 right = from_screen(s.x + 50.0, s.y, s.z) - rc;
   float l = glm::length(right);
   glm::vec3 dir = l > 0 ? right / l : glm::vec3(1, 0, 0);
   end_a = rc - 5.0f * dir;
   end_b = rc + 5.0f * dir;
   placed = true;
   dirty = true;
}

// what is under the pointer: an atom, else the density peak along the line of sight,
// else the point at the depth of the rotation centre
glm::vec3 point_under_pointer(double x, double y) {
   graphics_info_t g;
   graphics_info_t::mouse_current_x = x;
   graphics_info_t::mouse_current_y = y;
   pick_info pi = g.atom_pick_gtk3(false);
   if (pi.success && is_valid_model_molecule(pi.imol)) {
      mmdb::Atom *at = graphics_info_t::molecules[pi.imol].atom_sel.atom_selection[pi.atom_index];
      if (at) return glm::vec3(at->x, at->y, at->z);
   }
   int imap = imol_refinement_map(); // c-interface.h
   if (is_valid_map_molecule(imap) && graphics_info_t::molecules[imap].is_displayed_p()) {
      glm::vec3 f = from_screen(x, y, -1.0f), b = from_screen(x, y, 1.0f);
      try {
         clipper::Coord_orth peak = graphics_info_t::molecules[imap].find_peak_along_line_favour_front(
            clipper::Coord_orth(f.x, f.y, f.z), clipper::Coord_orth(b.x, b.y, b.z));
         return glm::vec3(peak.x(), peak.y(), peak.z());
      }
      catch (const std::runtime_error &) { }
   }
   return from_screen(x, y, to_screen(rotation_centre()).z);
}

void add_ball(const glm::vec3 &c, float r) {
   // a small octahedron (enough at this size)
   const glm::vec3 d[6] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
   const unsigned int f[8][3] = {{0,2,4},{2,1,4},{1,3,4},{3,0,4},{2,0,5},{1,2,5},{3,1,5},{0,3,5}};
   std::vector<s_generic_vertex> v;
   std::vector<g_triangle> t;
   for (unsigned int i=0; i<8; i++)
      for (unsigned int j=0; j<3; j++) {
         glm::vec3 n = d[f[i][j]];
         v.push_back(s_generic_vertex(c + r * n, n, ruler_colour));
      }
   for (unsigned int i=0; i<8; i++) t.push_back(g_triangle(3*i, 3*i+1, 3*i+2));
   mesh.import(v, t);
}

void rebuild_mesh() {
   mesh.clear();
   float l = glm::length(end_b - end_a);
   if (l > 0.01f) {
      cylinder c(std::make_pair(end_a, end_b), 0.05, 0.05, l, ruler_colour, 8, 2);
      std::vector<s_generic_vertex> v;
      for (const auto &vv : c.vertices) v.push_back(s_generic_vertex(vv.pos, vv.normal, vv.color));
      mesh.import(v, c.triangles);
   }
   add_ball(end_a, 0.28f);
   add_ball(end_b, 0.28f);
   Material m;
   mesh.setup(m);
   dirty = false;
}

double distance_to_segment(const glm::vec2 &p, const glm::vec2 &a, const glm::vec2 &b) {
   glm::vec2 ab = b - a;
   float l2 = glm::dot(ab, ab);
   float t = l2 > 0 ? glm::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
   return glm::length(p - (a + t * ab));
}

// a "click an atom" mode is waiting for a click: that click is not for the ruler
bool pick_pending() {
   using g = graphics_info_t;
   return g::in_range_define || g::in_pepflip_define || g::in_rigid_body_define || g::in_terminal_residue_define ||
      g::in_rot_trans_object_define || g::in_residue_info_define || g::in_distance_define || g::in_angle_define ||
      g::in_torsion_define || g::in_rotamer_define || g::in_mutate_define || g::in_mutate_auto_fit_define ||
      g::in_auto_fit_define || g::in_db_main_define || g::in_edit_phi_psi_define || g::in_add_alt_conf_define ||
      g::in_save_symmetry_define || g::in_cis_trans_convert_define || g::in_180_degree_flip_define ||
      g::in_reverse_direction_define || g::in_dynamic_distance_define || g::in_torsion_general_define ||
      g::in_multi_residue_torsion_define || g::in_edit_chi_angles_define || g::in_user_defined_define ||
      g::delete_item_atom || g::delete_item_water || g::delete_item_residue || g::delete_item_residue_zone ||
      g::delete_item_sidechain || g::delete_item_residue_hydrogens;
}

void changed() {
   dirty = true;
   graphics_info_t::graphics_draw();
}

} // namespace

void coot_plus_ruler_set_on(bool state) {
   on = state;
   grabbed = -1;
   if (on) {
      // first time, or it is far from what you are looking at: put it in the middle of the view
      if (!placed || glm::length(0.5f * (end_a + end_b) - rotation_centre()) > 40.0f)
         place_at_centre();
   }
   changed();
}

bool coot_plus_ruler_is_on() { return on; }

double coot_plus_ruler_length() { return glm::length(end_b - end_a); }

void coot_plus_ruler_set_ends(float x1, float y1, float z1, float x2, float y2, float z2) {
   end_a = glm::vec3(x1, y1, z1);
   end_b = glm::vec3(x2, y2, z2);
   placed = true;
   changed();
}

void coot_plus_ruler_reset() {
   place_at_centre();
   changed();
}

// COOT+ hook: called from graphics_info_t::draw_measure_distance_and_angles()
void coot_plus_ruler_draw(stereo_eye_t eye) {
   if (!on) return;
   if (dirty) rebuild_mesh();
   graphics_info_t g;
   glm::mat4 m = graphics_info_t::get_molecule_mvp(eye);
   glm::mat4 rot = graphics_info_t::get_model_rotation();
   glm::vec4 bg(graphics_info_t::background_colour, 1.0);
   glm::vec3 rc = rotation_centre();
   mesh.draw(&graphics_info_t::shader_for_moleculestotriangles, eye, m, rot, graphics_info_t::lights,
             graphics_info_t::eye_position, rc, 1.0f, bg, false, graphics_info_t::shader_do_depth_fog_flag, false);

   char buf[32];
   snprintf(buf, sizeof(buf), "%.1f", coot_plus_ruler_length());
   std::string label = buf;
   unsigned char a_ring = 197; // how Coot's label font writes the A-ring (see add_measure_distance())
   label += " ";
   label += a_ring;
   glm::vec3 mid = 0.5f * (end_a + end_b) + glm::vec3(0.2, 0.2, 0.2);
   graphics_info_t::tmesh_for_labels.draw_atom_label(label, mid, ruler_colour, &graphics_info_t::shader_for_atom_labels,
                                                    eye, m, rot, bg, graphics_info_t::shader_do_depth_fog_flag,
                                                    graphics_info_t::perspective_projection_flag);
}

// COOT+ hook: on_glarea_drag_begin_primary(). Grab an end (or the line) if the press is on it.
bool coot_plus_ruler_drag_begin(double x, double y) {
   grabbed = -1;
   if (!on) return false;
   glm::vec3 sa = to_screen(end_a), sb = to_screen(end_b);
   glm::vec2 p(x, y), pa(sa.x, sa.y), pb(sb.x, sb.y);
   double da = glm::length(p - pa), db = glm::length(p - pb);
   const double grab_px = 14.0;
   if (da < grab_px && da <= db) { grabbed = 0; grab_depth = sa.z; }
   else if (db < grab_px)        { grabbed = 1; grab_depth = sb.z; }
   else if (distance_to_segment(p, pa, pb) < 8.0) {
      grabbed = 2;
      grab_depth = to_screen(0.5f * (end_a + end_b)).z;
      glm::vec3 hit = from_screen(x, y, grab_depth);
      grab_offset_a = end_a - hit;
      grab_offset_b = end_b - hit;
   }
   return grabbed >= 0;
}

// COOT+ hook: on_glarea_drag_update_primary(), x y are the pointer position (not the offset)
bool coot_plus_ruler_drag_update(double x, double y) {
   if (!on || grabbed < 0) return false;
   glm::vec3 w = from_screen(x, y, grab_depth);   // stays at the same depth while you drag
   if (grabbed == 0) end_a = w;
   if (grabbed == 1) end_b = w;
   if (grabbed == 2) { end_a = w + grab_offset_a; end_b = w + grab_offset_b; }
   changed();
   return true;
}

// COOT+ hook: on_glarea_drag_end_primary(). A click (no drag) moves the nearer end to
// the atom / density under the pointer.
bool coot_plus_ruler_drag_end(double x, double y, bool was_click) {
   if (!on) return false;
   if (grabbed >= 0) { grabbed = -1; changed(); return true; }
   if (!was_click || pick_pending()) return false;
   glm::vec3 p = point_under_pointer(x, y);
   glm::vec3 sa = to_screen(end_a), sb = to_screen(end_b);
   double da = glm::length(glm::vec2(x, y) - glm::vec2(sa.x, sa.y));
   double db = glm::length(glm::vec2(x, y) - glm::vec2(sb.x, sb.y));
   if (da <= db) end_a = p; else end_b = p;
   changed();
   return true;
}

// scripting API (c-interface.h)
void  set_coot_plus_ruler(int state) { coot_plus_ruler_set_on(state != 0); }
int   get_coot_plus_ruler() { return coot_plus_ruler_is_on() ? 1 : 0; }
float coot_plus_ruler_distance() { return coot_plus_ruler_length(); }
float coot_plus_ruler_end_coord(int end, int axis) {
   const glm::vec3 &e = end == 0 ? end_a : end_b;
   return (axis >= 0 && axis < 3) ? e[axis] : 0.0f;
}
void  set_coot_plus_ruler_ends(float x1, float y1, float z1, float x2, float y2, float z2) {
   coot_plus_ruler_set_ends(x1, y1, z1, x2, y2, z2);
}
