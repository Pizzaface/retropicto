"""Locate the source package for repository-only tool launchers."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
