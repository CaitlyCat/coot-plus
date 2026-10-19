/*
 * utils/setup-syminfo.cc
 *
 * Copyright 2009 by University of Oxford
 * Author: Paul Emsley
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 3 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */


#include <iostream>
#include <string.h>
#include <string>
#include <vector>
#include <stdlib.h>

#include "coot-utils.hh"
#include "setup-syminfo.hh"
#ifdef _MSC_VER
  // bypass on Windows - thank you Charles Ballard
#else
// for stat()
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Coot+: return true if a usable symmetry library was found (and SYMINFO now
// points at it). Previously this function only printed to the terminal, and a
// SYMINFO pointing at a missing file (as the CCP4 9 macOS wrapper does) was
// accepted silently - so symmetry just did nothing.
bool setup_syminfo() {

   // How should the setting up of symmetry work?
   //
   // First we check the environment variable SYMINFO (and that the file exists).
   //
   // If that is not set, then we look in the standard place: $prefix/share/coot/data,
   // where $prefix is found from the compile-time PKGDATADIR or the location of the
   // executable (see coot::package_data_dir()), then in CCP4's $CLIBD.
   //
   // If all these fail then give an error message.

   auto file_exists = [] (const std::string &fn) {
      struct stat buf;
      return stat(fn.c_str(), &buf) == 0;
   };

   char *syminfo = getenv("SYMINFO");
   if (syminfo) {
      if (file_exists(syminfo))
         return true;
      std::cout << "WARNING:: SYMINFO is set to " << syminfo
                << " but that file does not exist - looking elsewhere" << std::endl;
   }

   std::vector<std::string> candidates;
   std::string pkgdatadir = coot::package_data_dir();
   candidates.push_back(pkgdatadir + "/data/syminfo.lib");
   candidates.push_back(pkgdatadir + "/syminfo.lib");
   char *clibd = getenv("CLIBD");
   if (clibd)
      candidates.push_back(std::string(clibd) + "/syminfo.lib");
   char *ccp4 = getenv("CCP4");
   if (ccp4)
      candidates.push_back(std::string(ccp4) + "/lib/data/syminfo.lib");

   for (const auto &fn : candidates) {
      if (file_exists(fn)) {
         std::string symstring = "SYMINFO=" + fn;
         // Mind bogglingly enough, the string is part of the environment
         // and malleable, so const char * of a local variable is not
         // what we want at all.
         //
         // We fire and forget, we don't want to change s.
         //
         char *s = new char[symstring.length() + 1];
         strcpy(s, symstring.c_str());
         putenv(s);
         return true;
      }
   }

   std::cout << "WARNING:: Symmetry library (syminfo.lib) not found at "
             << candidates[0] << " and environment variable SYMINFO is not set"
             << (syminfo ? " to an existing file." : ".") << std::endl;
   std::cout << "WARNING:: Symmetry will not be possible\n";
   return false;
}
