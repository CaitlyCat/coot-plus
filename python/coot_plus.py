# coot_plus.py
#
# Coot+ analysis helpers used by the Coot+ interface (src/coot-plus-ui.cc).
# Everything here calls the existing coot / coot_utils functions; the results
# are plain lists/dicts so they can be handed to the interface as JSON.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or (at
# your option) any later version.

import os
import re
import coot
import coot_utils

# ------------------------------------------------------------------------------
# validation: per-residue problems
# ------------------------------------------------------------------------------

# Coot's Ramachandran values are probability densities (not percentiles). The
# thresholds are the ones Coot 1's Ramachandran plot uses (src/gl-rama-plot.cc:
# preferred 0.02, allowed 0.002): below 0.002 is outside the allowed region.
RAMA_OUTLIER = 0.002
RAMA_ALLOWED = 0.02
ROTAMER_OUTLIER = 1.0      # rotamer_graphs_py gives a percentage likelihood
DENSITY_POOR = 0.6         # per-residue map-model correlation
NO_ROTAMERS = ("ALA", "GLY")


def _key(spec):
    return (spec[0], spec[1], spec[2])


def validation_issues(imol, imol_map=-1):
    """Residues with problems: Ramachandran, rotamer, density fit.
    Returns a list of dicts sorted worst first:
    {"chain","resno","ins","resname","severity" (1 or 2),"problems":[text...]}"""
    issues = {}

    def add(spec, resname, severity, text):
        k = _key(spec)
        d = issues.setdefault(k, {"chain": spec[0], "resno": spec[1], "ins": spec[2],
                                  "resname": resname, "severity": 0, "problems": []})
        d["severity"] = max(d["severity"], severity)
        d["problems"].append(text)
        if not d["resname"]:
            d["resname"] = resname

    rs = coot.all_molecule_ramachandran_score_py(imol)
    if isinstance(rs, list) and len(rs) > 5 and isinstance(rs[5], list):
        for item in rs[5]:
            try:
                phipsi, spec, prob, names = item[0], item[1], item[2], item[3]
            except (IndexError, TypeError):
                continue
            resname = names[1] if isinstance(names, list) and len(names) > 1 else ""
            if prob < RAMA_OUTLIER:
                add(spec, resname, 2, "Ramachandran outlier")
            elif prob < RAMA_ALLOWED:
                add(spec, resname, 1, "Ramachandran: allowed, not favoured")

    rg = coot.rotamer_graphs_py(imol)
    if isinstance(rg, list):
        for item in rg:
            try:
                chain, resno, ins, score, resname = item[:5]
            except (ValueError, TypeError):
                continue
            if resname in NO_ROTAMERS:
                continue
            if score < ROTAMER_OUTLIER:
                add([chain, resno, ins], resname, 2, "Rotamer outlier (%.1f%%)" % score)

    if imol_map >= 0 and coot.is_valid_map_molecule(imol_map):
        specs = coot_utils.all_residues(imol) or []
        protein = [s for s in specs if coot.residue_name(imol, s[0], s[1], s[2]) not in ("HOH", "WAT")]
        corr = coot.map_to_model_correlation_per_residue_py(imol, protein, 0, imol_map)
        if isinstance(corr, list):
            for item in corr:
                try:
                    spec, c = item[0], item[1]
                except (IndexError, TypeError):
                    continue
                if c < DENSITY_POOR:
                    add(spec, coot.residue_name(imol, spec[0], spec[1], spec[2]),
                        2 if c < 0.4 else 1, "Poor density fit (CC %.2f)" % c)

    out = list(issues.values())
    out.sort(key=lambda d: (-d["severity"], d["chain"], d["resno"]))
    return out


def draw_validation_markers(imol, imol_map=-1):
    """Mark problem residues in the 3D view (generic display objects).
    Returns [object numbers, number of residues marked]."""
    if not coot.use_graphics_interface_state():
        return [[-1, -1], 0]   # 3D markers need the graphics window (crashes without it)
    issues = validation_issues(imol, imol_map)
    name_bad = "Coot+ problems (serious) #%d" % imol
    name_warn = "Coot+ problems (minor) #%d" % imol
    objs = []
    for name in (name_bad, name_warn):
        old = coot.generic_object_index(name)
        if old >= 0:
            coot.close_generic_object(old)
    o_bad = coot.new_generic_object_number(name_bad)
    o_warn = coot.new_generic_object_number(name_warn)
    n = 0
    for d in issues:
        c = coot.residue_centre_py(imol, d["chain"], d["resno"], d["ins"])
        if not c:
            continue
        if d["severity"] >= 2:
            coot.to_generic_object_add_point(o_bad, "red", 28, c[0], c[1], c[2])
        else:
            coot.to_generic_object_add_point(o_warn, "orange", 20, c[0], c[1], c[2])
        n += 1
    coot.set_display_generic_object(o_bad, 1)
    coot.set_display_generic_object(o_warn, 1)
    return [[o_bad, o_warn], n]


def clear_validation_markers(imol):
    for name in ("Coot+ problems (serious) #%d" % imol, "Coot+ problems (minor) #%d" % imol):
        i = coot.generic_object_index(name)
        if i >= 0:
            coot.close_generic_object(i)

# ------------------------------------------------------------------------------
# unmodelled blobs with their neighbours
# ------------------------------------------------------------------------------

def blobs(imol, imol_map, sigma=1.0, near_radius=5.0):
    """Unmodelled density blobs, biggest first:
    {"x","y","z","score","near":[["A",12,"","HIS"],...]}"""
    b = coot.find_blobs_py(imol, imol_map, sigma)
    out = []
    if not isinstance(b, list):
        return out
    for item in b:
        try:
            pos, score = item[0], item[1]
        except (IndexError, TypeError):
            continue
        near = coot.residues_near_position_py(imol, pos, near_radius) or []
        near_named = []
        for spec in near[:4]:
            near_named.append([spec[0], spec[1], spec[2], coot.residue_name(imol, spec[0], spec[1], spec[2])])
        out.append({"x": pos[0], "y": pos[1], "z": pos[2], "score": score, "near": near_named})
    out.sort(key=lambda d: -d["score"])
    return out

# ------------------------------------------------------------------------------
# ligand check
# ------------------------------------------------------------------------------

def ligand_info(imol, chain, resno, ins, imol_map=-1, imol_protein=-1):
    """Density fit and contacts of a ligand. Contacts come from the ligand's own
    molecule and, if the ligand is a separate molecule, from imol_protein too."""
    spec = [chain, resno, ins]
    info = {"resname": coot.residue_name(imol, chain, resno, ins)}
    if imol_map >= 0 and coot.is_valid_map_molecule(imol_map):
        corr = coot.map_to_model_correlation_per_residue_py(imol, [spec], 0, imol_map)
        if isinstance(corr, list) and corr:
            info["cc"] = corr[0][1]
    near = [[s[0], s[1], s[2], coot.residue_name(imol, s[0], s[1], s[2])]
            for s in (coot.residues_near_residue_py(imol, spec, 4.0) or [])]
    if imol_protein >= 0 and imol_protein != imol and coot.is_valid_model_molecule(imol_protein):
        # residues of the protein within ~4 A of any ligand atom: search around the
        # ligand centre, then keep residues with an atom close to a ligand atom
        centre = coot.residue_centre_py(imol, chain, resno, ins)
        lig_atoms = coot.residue_info_py(imol, chain, resno, ins) or []
        lig_xyz = [a[2] for a in lig_atoms if len(a) > 2]
        if centre:
            for s in coot.residues_near_position_py(imol_protein, centre, 10.0) or []:
                atoms = coot.residue_info_py(imol_protein, s[0], s[1], s[2]) or []
                close = False
                for a in atoms:
                    p = a[2]
                    for q in lig_xyz:
                        if (p[0]-q[0])**2 + (p[1]-q[1])**2 + (p[2]-q[2])**2 < 16.0:
                            close = True
                            break
                    if close:
                        break
                if close:
                    near.append([s[0], s[1], s[2], coot.residue_name(imol_protein, s[0], s[1], s[2])])
    info["contacts"] = near[:16]
    info["n_waters_near"] = len([c for c in info["contacts"] if c[3] in ("HOH", "WAT")])
    return info

# ------------------------------------------------------------------------------
# Phenix refinement runs
# ------------------------------------------------------------------------------

_RUN_RE = re.compile(r"^(?P<stem>.*?)_?(?:refine|Refine)_(?P<n>\d+)\.(?P<ext>pdb|cif)$")


def phenix_runs(folder):
    """Refinement runs in a folder (and its Refine_* sub-folders), newest last:
    {"model","mtz","run","folder","mtime"}"""
    runs = []
    if not os.path.isdir(folder):
        return runs
    dirs = [folder] + [os.path.join(folder, d) for d in sorted(os.listdir(folder))
                       if os.path.isdir(os.path.join(folder, d))]
    for d in dirs:
        try:
            names = os.listdir(d)
        except OSError:
            continue
        for f in names:
            m = _RUN_RE.match(f)
            if not m:
                continue
            base = f[: -len(m.group("ext")) - 1]
            mtz = os.path.join(d, base + ".mtz")
            if not os.path.exists(mtz):
                continue
            model = os.path.join(d, f)
            runs.append({"model": model, "mtz": mtz, "run": int(m.group("n")),
                         "folder": d, "mtime": os.path.getmtime(model)})
    # one entry per run: prefer .pdb next to .cif
    seen = {}
    for r in runs:
        k = (r["folder"], r["run"])
        if k not in seen or r["model"].endswith(".pdb"):
            seen[k] = r
    out = list(seen.values())
    out.sort(key=lambda r: (r["mtime"], r["run"]))
    return out


def open_phenix_run(model, mtz):
    """Open a refinement run: the model and the maps from the MTZ (Coot picks the
    2mFo-DFc and mFo-DFc columns). Returns [imol_model, [map numbers]]."""
    imol = coot.handle_read_draw_molecule_with_recentre(model, 1)
    maps = coot.auto_read_make_and_draw_maps(mtz)
    if isinstance(maps, tuple):
        maps = list(maps)
    elif not isinstance(maps, list):
        maps = [maps]
    return [imol, maps]

# ------------------------------------------------------------------------------
# ready-made snippets for the Python console (each one is run in the tests)
# ------------------------------------------------------------------------------

SNIPPETS = [
    ("Label all CA atoms",
     "Labels the CA atom of every residue in the model at the centre",
     """imol = coot.active_residue_py()[0]
for chain, resno, ins in coot_utils.all_residues(imol):
    coot.add_atom_label(imol, chain, resno, " CA ")
"""),
    ("Waters near the centre residue",
     "Lists the waters within 5 A of the residue at the centre",
     """ar = coot.active_residue_py()
imol, spec = ar[0], [ar[1], ar[2], ar[3]]
for s in coot.residues_near_residue_py(imol, spec, 5.0):
    if coot.residue_name(imol, s[0], s[1], s[2]) == "HOH":
        print("water", s[0], s[1])
"""),
    ("B-factor summary",
     "Average and median B-factor of every open model",
     """for imol in coot_utils.molecule_number_list():
    if coot.is_valid_model_molecule(imol):
        print(coot.molecule_name(imol))
        print("   average B %.1f   median B %.1f" % (coot.average_temperature_factor(imol), coot.median_temperature_factor(imol)))
"""),
    ("Residues with alt confs",
     "Lists the residues that have alternative conformations in the model at the centre",
     """imol = coot.active_residue_py()[0]
for r in coot_utils.residues_with_alt_confs(imol):
    print(r)
"""),
    ("Residues per chain",
     "Counts the residues in each chain of the model at the centre",
     """imol = coot.active_residue_py()[0]
for chain in coot_utils.chain_ids(imol):
    print(chain, len(coot_utils.residues_in_chain(imol, chain)), "residues")
"""),
    ("Go to the worst-fitting residue",
     "Centres on the residue with the most serious problem (Ramachandran, rotamer, density)",
     """import coot_plus
imol = coot.active_residue_py()[0]
issues = coot_plus.validation_issues(imol, coot.imol_refinement_map())
if issues:
    d = issues[0]
    coot.set_go_to_atom_molecule(imol)
    coot.set_go_to_atom_chain_residue_atom_name(d["chain"], d["resno"], " CA ")
    print(d["chain"], d["resno"], d["resname"], "-", ", ".join(d["problems"]))
"""),
    ("Save model as mmCIF",
     "Writes the model at the centre as an mmCIF file next to the original",
     """import os
imol = coot.active_residue_py()[0]
name = os.path.splitext(coot.molecule_name(imol))[0] + "-coot.cif"
coot.write_cif_file(imol, name)
print("saved", name)
"""),
    ("Go to a residue (edit me)",
     "Change the chain and number, then Run",
     """chain, resno = "A", 42
imol = coot.active_residue_py()[0]
coot.set_go_to_atom_molecule(imol)
coot.set_go_to_atom_chain_residue_atom_name(chain, resno, " CA ")
"""),
]


def snippets():
    return [{"title": t, "description": d, "code": c} for (t, d, c) in SNIPPETS]
