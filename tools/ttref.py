"""Loads the PC game in reference/ (terminaltanks.py + the tanks/ package, unmodified) as one flat module `tt`.

Since 2.0.0 the PC game is a package. load() imports every module and returns a namespace holding every top-level name
the package defines, taken from the module that defines it, so the generators and parity tests can keep reading
`tt.ALL_AMMOS`, `tt.World` and so on. A few names that 2.0.0 renamed or inlined are aliased at the bottom."""
import ast, importlib, os, pkgutil, sys, types

REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "reference")
ENTRY = os.path.join(REF, "terminaltanks.py")


def _top_level_names(path):
    out = set()
    for node in ast.parse(open(path, encoding="utf-8").read()).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            out.add(node.name)
        elif isinstance(node, ast.Assign):
            for t in node.targets:
                for n in ast.walk(t):
                    if isinstance(n, ast.Name):
                        out.add(n.id)
        elif isinstance(node, (ast.AnnAssign, ast.AugAssign)) and isinstance(node.target, ast.Name):
            out.add(node.target.id)
    return out


OLD_NAMES = {"_flick": "flicker", "_ELEM": "ELEMENTS", "_PHASE_ELEMS": "PHASE_ELEMENTS", "_MOTE_STYLES": "MOTE_STYLES",
             "_to_256": "to_256", "_nearest16": "nearest16", "_parse_cli": "parse_cli", "_reset_progress": "reset_progress",
             "_NOTE_NAMES": "NOTE_NAMES", "_SCALE_NAMES": "SCALE_NAMES", "_DIGITS": "DIGITS", "_au_inst_table": "au_inst_table",
             "_UNLOCKABLES": "UNLOCKABLES", "_family_of": "family_of", "_TURRETS": "TURRETS", "_HULLS": "HULLS", "_TRACKS": "TRACKS"}


def load():
    if "tt" in sys.modules:
        return sys.modules["tt"]
    os.environ.setdefault("TT_NOAUDIO", "1")
    os.environ.setdefault("TT_AUDIO_SINK", "none")
    if REF not in sys.path:
        sys.path.insert(0, REF)
    pkg = importlib.import_module("tanks")
    modules = [pkg] + [importlib.import_module(m.name) for m in pkgutil.walk_packages(pkg.__path__, "tanks.")]
    tt = types.ModuleType("tt")
    for m in modules:
        for name in sorted(_top_level_names(m.__file__)):
            if hasattr(m, name) and not hasattr(tt, name):
                setattr(tt, name, getattr(m, name))
    for old, new in OLD_NAMES.items():
        if hasattr(tt, new) and not hasattr(tt, old):
            setattr(tt, old, getattr(tt, new))
    tt.__version__ = pkg.__version__
    tt.__modules__ = modules
    sys.modules["tt"] = tt
    return tt
