import subprocess
from pathlib import Path

SCRIPT = Path(__file__).parents[1] / "scripts/check-collapse-anchor-gate.py"

def test_gate_script_has_six_decision_check():
    text = SCRIPT.read_text()
    assert "range(1, 7)" in text
    assert "diff" in text

def test_gate_script_is_executable_python():
    result = subprocess.run(["python3", str(SCRIPT)], capture_output=True, text=True)
    assert result.returncode == 0
