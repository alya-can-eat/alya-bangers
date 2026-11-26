import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))

import fixtures  # noqa: E402


@pytest.fixture(scope="session")
def library(tmp_path_factory):
    """A small library with the awkward cases of a real one (see fixtures.py)."""
    root = tmp_path_factory.mktemp("lib")
    facts = fixtures.build_library(root)
    return root, facts
