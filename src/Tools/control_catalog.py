"""Read the authoritative C++ action catalog for tools and generated docs."""
from pathlib import Path
import argparse
import re

ROOT = Path(__file__).resolve().parents[2]
PATTERN = re.compile(
    r'\{"(request_\w+)", "([^"]+)", Capability::(\w+),[^\n]+\n'
    r'\s*"([^"]*)", "([^"]*)", "([^"]*)"(, false)?\}'
)
def load_catalog():
    records = PATTERN.findall((ROOT / "src/Ai/ControlAction.cpp").read_text())
    if not records:
        raise RuntimeError("C++ control catalog cannot be read")
    return {
        name: dict(signature=signature, capability=capability, description=description,
                   completion=completion, failure=failure)
        for name, signature, capability, description, completion, failure, disabled in records
        if not disabled
    }
CATALOG = load_catalog()
def validate_action(name, arguments):
    if name not in CATALOG:
        raise ValueError("unsupported_action")
    signature = CATALOG[name]["signature"]
    required = [key.strip() for key in signature.partition("(")[2].rstrip(")").split(",") if key.strip()]
    if not isinstance(arguments, dict) or set(arguments) != set(required):
        raise ValueError("invalid_arguments")
    for key, value in arguments.items():
        if key in ("entry_id", "quest_id", "nav_epoch"):
            if type(value) is not int or not (0 <= value <= 0xFFFFFFFF) or (key != "nav_epoch" and value == 0):
                raise ValueError("invalid_arguments")
        elif not isinstance(value, str) or not value:
            raise ValueError("invalid_arguments")
    if "skill" in required and (arguments["skill"] != "fishing" or arguments["intent"] != "fish"):
        raise ValueError("unsupported_profession_intent")
def generated_table():
    lines = ["<!-- control-catalog:start -->", "| Tool | Completion |", "| --- | --- |"]
    lines.extend(f"| `{entry['signature']}` | {entry['completion']} |" for entry in CATALOG.values())
    lines.append("<!-- control-catalog:end -->")
    return "\n".join(lines)
if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    readme = ROOT / "README.md"
    content = readme.read_text()
    pattern = r"<!-- control-catalog:start -->.*?<!-- control-catalog:end -->"
    match = re.search(pattern, content, re.S)
    if not match:
        raise SystemExit("README catalog markers are missing")
    table = generated_table()
    if args.check and match.group() != table:
        raise SystemExit("README catalog is stale")
    if args.write:
        readme.write_text(content[:match.start()] + table + content[match.end():])
