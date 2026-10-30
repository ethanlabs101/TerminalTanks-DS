"""Loads the original terminaltanks.py (unmodified) as module `tt` for extraction and parity tests."""
import importlib.util, os, sys
REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "reference", "terminaltanks.py")

def load():
    if "tt" in sys.modules:
        return sys.modules["tt"]
    spec = importlib.util.spec_from_file_location("tt", REF)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["tt"] = mod
    spec.loader.exec_module(mod)
    return mod
