"""Pure policy helpers shared by the UE mesh importer and stdlib specs."""


def should_skip_stripped_curvy(name, stripped):
    return bool(stripped) and str(name).casefold().endswith("_curvy")
