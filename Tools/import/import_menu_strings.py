"""Generate and import the shipped menu-localization registry.

Parses all 15 [menu*] sections in original/System/WoT.int, writes the
deterministic CSV source, and creates:
  /Game/WoT/UI/Menus/DA_WoTMenuStrings

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script=".../Tools/import/import_menu_strings.py" -unattended -nullrhi

Completion is recorded in Saved/import_menu_strings_result.json because
commandlet stdout is not a reliable completion signal on the parent hosts.
"""
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Tools"))
from wot_config import original_dir  # noqa: E402
CSV_PATH = REPO / "Tools" / "import" / "data" / "menu_strings.csv"
RESULT_FILE = REPO / "Saved" / "import_menu_strings_result.json"
DESTINATION = "/Game/WoT/UI/Menus"
ASSET_NAME = "DA_WoTMenuStrings"
ASSET_PATH = f"{DESTINATION}/{ASSET_NAME}"
EXPECTED_SECTION_COUNT = 15
VERIFICATION_PIN_KEYS = (
    "menuMain.MenuList[2]",
    "menuMain.MenuList[7]",
    "menuMultiPlayer.HelpMessage[1]",
    "menuLoad.RestartString",
)


def _unquote(value):
    # Whitespace immediately after '=' is syntax. For unquoted values,
    # everything after that (including retail-significant trailing spaces) is
    # data. Quoted values may have syntax whitespace after their closing quote.
    value = value.lstrip()
    quoted = value.rstrip()
    if len(quoted) >= 2 and quoted[0] == '"' and quoted[-1] == '"':
        return quoted[1:-1].replace('""', '"')
    return value


def parse_menu_strings(path):
    sections = []
    rows = []
    current = None
    for line_number, raw in enumerate(
            path.read_text(encoding="cp1252").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith((";", "#")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            current = section if section.lower().startswith("menu") else None
            if current is not None:
                sections.append(current)
            continue
        if current is None:
            continue
        if "=" not in raw:
            raise ValueError(f"{path}:{line_number}: malformed menu entry")
        prop, value = raw.split("=", 1)
        prop = prop.strip()
        if not prop:
            raise ValueError(f"{path}:{line_number}: empty property")
        rows.append((f"{current}.{prop}", current, prop, _unquote(value)))

    if len(sections) != EXPECTED_SECTION_COUNT:
        raise ValueError(
            f"{path}: expected {EXPECTED_SECTION_COUNT} [menu*] sections, "
            f"found {len(sections)}")
    if len(set(sections)) != len(sections):
        raise ValueError(f"{path}: duplicate [menu*] section")
    keys = [row[0] for row in rows]
    if len(set(keys)) != len(keys):
        raise ValueError(f"{path}: duplicate menu string key")
    return sections, rows


def render_csv(rows):
    def field(value):
        value = str(value)
        quoted = (value[:1].isspace() or value[-1:].isspace()
                  or any(char in value for char in ',\"\r\n'))
        return f'"{value.replace(chr(34), chr(34) * 2)}"' if quoted else value

    output_rows = [("Key", "Section", "Property", "Text"), *rows]
    return "".join(
        ",".join(field(value) for value in row) + "\n"
        for row in output_rows)


def write_if_changed(path, text):
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="")
    return True


def write_result(result):
    RESULT_FILE.parent.mkdir(parents=True, exist_ok=True)
    RESULT_FILE.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def verify_imported_content(actual_strings, actual_sections, sections, rows,
                            name_factory):
    desired_strings = {
        name_factory(key): text for key, _, _, text in rows
    }
    desired_sections = [name_factory(section) for section in sections]
    expected_by_key = {key: text for key, _, _, text in rows}
    pin_results = {
        key: (key in expected_by_key
              and actual_strings.get(name_factory(key)) == expected_by_key[key])
        for key in VERIFICATION_PIN_KEYS
    }
    counts = {
        "keys": len(actual_strings),
        "sections": len(actual_sections),
    }
    ok = (counts["keys"] == len(rows)
          and counts["sections"] == len(sections)
          and actual_strings == desired_strings
          and actual_sections == desired_sections
          and all(pin_results.values()))
    return ok, pin_results, counts


def import_asset(sections, rows):
    import unreal

    eal = unreal.EditorAssetLibrary
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    asset_class = unreal.load_class(None, "/Script/WoT.WoTMenuStrings")
    if not asset_class:
        raise RuntimeError(
            "UWoTMenuStrings is not loaded; build the WoT editor module first")

    if not eal.does_directory_exist(DESTINATION):
        eal.make_directory(DESTINATION)
    created = not eal.does_asset_exist(ASSET_PATH)
    if created:
        factory = unreal.DataAssetFactory()
        factory.set_editor_property("data_asset_class", asset_class)
        asset = tools.create_asset(
            ASSET_NAME, DESTINATION, asset_class, factory)
    else:
        asset = unreal.load_asset(ASSET_PATH)
    if not asset:
        raise RuntimeError(f"failed to create or load {ASSET_PATH}")

    desired_strings = {unreal.Name(key): text for key, _, _, text in rows}
    desired_sections = [unreal.Name(section) for section in sections]
    changed = created
    if asset.get_editor_property("strings") != desired_strings:
        asset.set_editor_property("strings", desired_strings)
        changed = True
    if asset.get_editor_property("sections") != desired_sections:
        asset.set_editor_property("sections", desired_sections)
        changed = True
    if not eal.save_asset(ASSET_PATH):
        raise RuntimeError(f"save failed for {ASSET_PATH}")

    package = asset.get_outermost()
    reloaded, reload_error = unreal.EditorLoadingAndSavingUtils.reload_packages(
        [package], unreal.ReloadPackagesInteractionMode.ASSUME_NEGATIVE)
    if not reloaded:
        raise RuntimeError(
            f"post-save reload failed for {ASSET_PATH}: {reload_error}")

    # Result-file verification reads the reloaded package, including
    # representative retail pins, rather than trusting the live edited object.
    verified = unreal.load_asset(ASSET_PATH)
    if not verified:
        raise RuntimeError(f"post-save load failed for {ASSET_PATH}")
    actual_strings = verified.get_editor_property("strings")
    actual_sections = verified.get_editor_property("sections")
    ok, pin_results, counts = verify_imported_content(
        actual_strings, actual_sections, sections, rows, unreal.Name)
    if not ok:
        raise RuntimeError(
            f"post-save verification failed for {ASSET_PATH}: "
            f"pins={pin_results}, counts={counts}")
    return changed, pin_results, counts


def main():
    source = original_dir() / "System" / "WoT.int"
    result = {
        "asset": ASSET_PATH,
        "csv": str(CSV_PATH),
        "status": "running",
    }
    write_result(result)
    try:
        sections, rows = parse_menu_strings(source)
        result["sections"] = len(sections)
        result["rows"] = len(rows)
        result["csv_changed"] = write_if_changed(CSV_PATH, render_csv(rows))
        changed, pins, imported_counts = import_asset(sections, rows)
        result.update({
            "asset_changed": changed,
            "imported_counts": imported_counts,
            "pins": pins,
            "status": "ok",
        })
    except Exception as exc:
        result["status"] = "failed"
        result["error"] = str(exc)
        write_result(result)
        raise
    write_result(result)


if __name__ == "__main__":
    main()
