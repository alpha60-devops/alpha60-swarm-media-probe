import importlib.util
from pathlib import Path

SCRIPT = Path(__file__).parents[1] / "scripts" / "presumed_enrichment.py"
spec = importlib.util.spec_from_file_location("presumed_enrichment", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def test_output_name_appends_presumed():
    assert module.presumed_output_path(Path("result.json")) == Path("result.presumed.json")
    assert module.presumed_output_path(Path("result")) == Path("result.presumed.json")


def test_resolution_mapping():
    assert module.resolution_dimensions("1080p") == (1920, 1080)
    assert module.resolution_dimensions("unknown") == (None, None)
