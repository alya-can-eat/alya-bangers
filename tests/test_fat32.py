from dsconv.fat32 import fat_key, safe_component, utf16_len


def test_forbidden_characters_become_fullwidth():
    assert safe_component("VXLTRA - 00:00", ".dsa") == "VXLTRA - 00：00.dsa"
    assert safe_component('a*b?c"d<e>f|g\\h', "") == "a＊b？c＂d＜e＞f｜g＼h"


def test_dots_before_the_extension_are_kept():
    assert safe_component("Dear Wren...", ".dsa") == "Dear Wren....dsa"
    assert safe_component("n0m0n_ - ......", ".dsa") == "n0m0n_ - .......dsa"
    assert safe_component("Tin Owl Club - S.T.A.R.S.", ".dsa") == "Tin Owl Club - S.T.A.R.S..dsa"
    assert safe_component("(Extended Version) ", ".dsa") == "(Extended Version) .dsa"


def test_folder_names_cannot_end_in_dot_or_space():
    assert safe_component("Demos...", "") == "Demos\uFF0E\uFF0E\uFF0E"
    assert safe_component("  name . ", "") == "name \uFF0E"
    assert safe_component("...", "") == "\uFF0E\uFF0E\uFF0E"
    assert safe_component("   ", "") == "_"


def test_control_characters_dropped_and_nfc():
    assert safe_component("a\x01b\x7fc", "") == "abc"
    assert safe_component("Lúa", "") == "Lúa"


def test_reserved_device_names():
    assert safe_component("CON", ".dsa") == "CON_.dsa"
    assert safe_component("lpt1", "") == "lpt1_"
    assert safe_component("CONSOLE", "") == "CONSOLE"


def test_length_limit_counts_utf16_units_and_keeps_extension():
    name = "\U0001F3B5" * 200  # 400 UTF-16 units
    out = safe_component(name, ".dsa")
    assert out.endswith(".dsa")
    assert utf16_len(out) <= 255


def test_fat_key_is_case_insensitive_but_keeps_sharp_s():
    assert fat_key("Kivara/Song") == fat_key("KIVARA/song")
    assert fat_key("Straße") != fat_key("STRASSE")
