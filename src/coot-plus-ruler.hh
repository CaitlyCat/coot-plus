/*
 * src/coot-plus-ruler.hh
 *
 * Coot+ ruler: a 3D measuring line that is not attached to any molecule.
 * Drag either end (or the middle) to move it; a click moves the nearer end there
 * (onto the atom or the density under the pointer). It never changes a model.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or (at
 * your option) any later version.
 */

#ifndef COOT_PLUS_RULER_HH
#define COOT_PLUS_RULER_HH

#include "stereo-eye.hh"

void   coot_plus_ruler_set_on(bool state);   // shows it at the centre of the view the first time
bool   coot_plus_ruler_is_on();
double coot_plus_ruler_length();             // Angstroms
void   coot_plus_ruler_set_ends(float x1, float y1, float z1, float x2, float y2, float z2);
void   coot_plus_ruler_reset();              // back to 10 A across the centre of the view

// hooks (marked "// COOT+ hook" where they are called)
void coot_plus_ruler_draw(stereo_eye_t eye);                  // graphics-info-draw.cc
bool coot_plus_ruler_drag_begin(double x, double y);           // true: the ruler took the drag
bool coot_plus_ruler_drag_update(double x, double y);          // true: it is being dragged
bool coot_plus_ruler_drag_end(double x, double y, bool was_click); // true: handled

#endif // COOT_PLUS_RULER_HH
