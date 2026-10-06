"""Start the enrolled E2B plugin with its key read from a private file."""
import argparse
import os
from pathlib import Path

from bridge import read_secret


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--key-file", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    options = parser.parse_args()
    os.environ["VLLM_API_KEY"] = read_secret(options.key_file)
    os.execvp("aimee-gemma4-e2b", ["aimee-gemma4-e2b", "--root", str(options.root.resolve()),
                                "serve", "--config", str(options.config.resolve())])


if __name__ == "__main__":
    main()
