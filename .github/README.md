# Coot+

**An unofficial, reorganised interface for [Coot](https://github.com/pemsley/coot)**, the crystallography model-building program by Paul Emsley and colleagues. Coot+ is Coot 1 with a task-based layout and some extra tools. It opens the same files, and everything Coot does is still there.

**Download (macOS, Apple Silicon, macOS 15+):** [latest release](https://github.com/CaitlyCat/coot-plus/releases/latest). The zip has the app, a read-me and the source.

## What's different

- **Task sidebar:** Load, Navigate, Maps, Symmetry, Build, Ligands, Validate, Save. A short list of the tools you use most, plus all the tools grouped below. Right-click a tool to add it to your Common list.
- **Cmd-K search** for every tool and menu item. Type `A 42` to go to a residue.
- **Inspector:** a Molecules list you can label and colour. Each map row has its own contour and opacity, which you drag to change or click to type.
- **Selected residue card** with Previous/Next buttons and a clickable sequence strip; a ligand check card when you're centred on a ligand.
- **Ruler:** a 3D measuring line, not attached to any molecule. Drag its ends, or click to snap them onto atoms or density.
- **Validation on the model:** red and orange markers on the residues with problems, a Problems list and an Unmodelled Blobs list.
- **Find Ligands** window, **Make Assembly from Symmetry**, a **Phenix refinement loader**, workspaces, light/dark theme.
- **Python console** with a script editor and snippets; Undo View for display changes; a pick-mode banner (Esc cancels); a message strip for errors.
- Hover descriptions for vague controls (written from the source code), and one consistent set of icons.
- Fixes for some Coot 1 problems on the Mac. Symmetry library lookup, the Save Symmetry Coordinates dialog, Python entry results, label size, Shift-scroll zoom, dead and duplicated menu items.

## How it's made

Coot+ is official Coot (branch `main`) plus a series of patches. The branch [`coot-plus`](https://github.com/CaitlyCat/coot-plus/tree/coot-plus) is Coot plus those commits. The new interface lives in new files (`src/coot-plus-*`, `python/coot_plus.py`), connected to Coot at a few marked points (`// COOT+ hook`).

To build it yourself on a Mac with Homebrew, see `UPGRADING.md` in the release zip's Source folder.

## Licence

GPL v3, like Coot. No warranty. Coot+ isn't made or endorsed by the Coot developers; please report Coot+ problems here, not to them.
