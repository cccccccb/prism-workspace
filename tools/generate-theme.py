#!/usr/bin/env python3
"""Generate backend-independent, typed theme constants from the theme source."""
import argparse
import json
import math
import pathlib
import re


def cpp_name(name):
    return "k" + "".join(part[:1].upper() + part[1:] for part in name.split("_"))


def unique_object(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError(f"Duplicate theme key {name}")
        result[name] = value
    return result


def generate(source):
    data = json.loads(source.read_text(), object_pairs_hook=unique_object)
    if set(data) != {"format_version", "name", "numbers", "colors"} or data["format_version"] != 1:
        raise ValueError("Unsupported theme format")
    names = set()
    generated = set()
    for group in ("numbers", "colors"):
        if not isinstance(data[group], dict):
            raise ValueError(f"{group} must be an object")
        for name in data[group]:
            if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", name) or name in names or cpp_name(name) in generated:
                raise ValueError(f"Invalid or duplicate token {name}")
            names.add(name)
            generated.add(cpp_name(name))
    lines = ["// Generated from resources/themes/prism-glass.json; edit that source.",
             "#pragma once", '#include "prism/contracts/display_list.hpp"',
             "#include <array>", "#include <string_view>", "namespace prism::contracts::theme {",
             "inline constexpr unsigned kFormatVersion = 1;",
             "struct NumberToken { std::string_view name; double value; };",
             "struct ColorToken { std::string_view name; Color value; };"]
    for name, value in data["numbers"].items():
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
            raise ValueError(f"Invalid numeric theme token {name}")
        lines.append(f"inline constexpr double {cpp_name(name)} = {value!r};")
    for name, value in data["colors"].items():
        if not isinstance(value, str) or not re.fullmatch(r"#[0-9a-fA-F]{8}", value):
            raise ValueError(f"Invalid RGBA theme token {name}")
        channels = [int(value[index:index + 2], 16) for index in (1, 3, 5, 7)]
        lines.append(f"inline constexpr Color {cpp_name(name)}{{{', '.join(map(str, channels))}}};")
    lines.append(f"inline constexpr std::array<NumberToken, {len(data['numbers'])}> kNumbers{{{{")
    lines.extend(f'    {{"{name}", {cpp_name(name)}}},' for name in data["numbers"])
    lines.append("}};")
    lines.append(f"inline constexpr std::array<ColorToken, {len(data['colors'])}> kColors{{{{")
    lines.extend(f'    {{"{name}", {cpp_name(name)}}},' for name in data["colors"])
    lines.extend(["}};", "} // namespace prism::contracts::theme", ""])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    content = generate(args.source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != content:
        args.output.write_text(content)


if __name__ == "__main__":
    main()
