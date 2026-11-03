"""Parse the plug-in parameter definitions into the manifest that the preset
tooling validates roster values against.
"""

from __future__ import annotations
import hashlib
import json
import re
from pathlib import Path
from typing import Any


class GenerationError(RuntimeError):
    """Raised when a source contract cannot be parsed without guessing."""


def extract_balanced(
    text: str, start: int, opener: str, closer: str
) -> tuple[str, int]:
    """Return the contents and exclusive end of a balanced delimited region."""
    if start < 0 or not text.startswith(opener, start):
        raise GenerationError(f"expected {opener!r} at offset {start}")

    depth = 1
    index = start + len(opener)
    content_start = index
    quote: str | None = None
    escaped = False
    line_comment = False
    block_comment = False

    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""

        if line_comment:
            if char == "\n":
                line_comment = False
            index += 1
            continue
        if block_comment:
            if char == "*" and next_char == "/":
                block_comment = False
                index += 2
            else:
                index += 1
            continue
        if quote is not None:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
            index += 1
            continue
        if char == "/" and next_char == "/":
            line_comment = True
            index += 2
            continue
        if char == "/" and next_char == "*":
            block_comment = True
            index += 2
            continue
        if char in ('"', "'"):
            quote = char
            index += 1
            continue
        if text.startswith(opener, index):
            depth += 1
            index += len(opener)
            continue
        if text.startswith(closer, index):
            depth -= 1
            if depth == 0:
                return text[content_start:index], index + len(closer)
            index += len(closer)
            continue
        index += 1

    raise GenerationError(f"unterminated balanced region starting at offset {start}")


def split_top_level(text: str, delimiter: str = ",") -> list[str]:
    """Split text on a delimiter outside nested C++ constructs and comments."""
    fields: list[str] = []
    start = 0
    index = 0
    stack: list[str] = []
    pairs = {"(": ")", "{": "}", "[": "]", "<": ">"}
    quote: str | None = None
    escaped = False
    line_comment = False
    block_comment = False

    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if line_comment:
            if char == "\n":
                line_comment = False
            index += 1
            continue
        if block_comment:
            if char == "*" and next_char == "/":
                block_comment = False
                index += 2
            else:
                index += 1
            continue
        if quote is not None:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
            index += 1
            continue
        if char == "/" and next_char == "/":
            line_comment = True
            index += 2
            continue
        if char == "/" and next_char == "*":
            block_comment = True
            index += 2
            continue
        if char in ('"', "'"):
            quote = char
            index += 1
            continue

        if char in "({[":
            stack.append(pairs[char])
        elif char in ")}]":
            if not stack or stack[-1] != char:
                raise GenerationError(f"unbalanced delimiter {char!r} at offset {index}")
            stack.pop()
        elif char == "<":
            # Template brackets are only relevant when adjacent to identifiers.
            previous = text[index - 1] if index else ""
            if previous.isalnum() or previous in "_:>":
                stack.append(">")
        elif char == ">" and stack and stack[-1] == ">":
            stack.pop()
        elif text.startswith(delimiter, index) and not stack:
            fields.append(text[start:index].strip())
            index += len(delimiter)
            start = index
            continue
        index += 1

    if quote is not None or block_comment or stack:
        raise GenerationError("unterminated string, comment, or delimiter while splitting")
    fields.append(text[start:].strip())
    return fields


def extract_function_body(text: str, qualified_name: str) -> str:
    locator = mask_noncode(text)
    match = re.search(rf"\b{re.escape(qualified_name)}\s*\(", locator)
    if match is None:
        raise GenerationError(f"function not found: {qualified_name}")
    open_paren = locator.find("(", match.start())
    _, after_paren = extract_balanced(locator, open_paren, "(", ")")
    open_brace = locator.find("{", after_paren)
    if open_brace < 0:
        raise GenerationError(f"function body not found: {qualified_name}")
    _, end = extract_balanced(locator, open_brace, "{", "}")
    return text[open_brace + 1 : end - 1]


def mask_noncode(
    text: str, *, mask_strings: bool = True, hash_comments: bool = False
) -> str:
    """Mask comments and optionally literals without changing offsets."""
    output = list(text)

    def blank(start: int, end: int, keep: set[int] | None = None) -> None:
        keep = keep or set()
        for position in range(start, end):
            if position not in keep and text[position] not in "\r\n":
                output[position] = " "

    index = 0
    while index < len(text):
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if text.startswith("//", index):
            end = text.find("\n", index + 2)
            end = len(text) if end < 0 else end
            blank(index, end)
            index = end
            continue
        if text.startswith("/*", index):
            close = text.find("*/", index + 2)
            end = len(text) if close < 0 else close + 2
            blank(index, end)
            index = end
            continue
        if hash_comments and text[index] == "#":
            end = text.find("\n", index + 1)
            end = len(text) if end < 0 else end
            blank(index, end)
            index = end
            continue

        raw_match = re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(', text[index:])
        if raw_match:
            delimiter = raw_match.group(1)
            close_token = f'){delimiter}"'
            close = text.find(close_token, index + raw_match.end())
            end = len(text) if close < 0 else close + len(close_token)
            if mask_strings:
                blank(index, end)
            index = end
            continue

        if text[index] in ('"', "'"):
            quote = text[index]
            start = index
            index += 1
            escaped = False
            while index < len(text):
                char = text[index]
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quote:
                    index += 1
                    break
                index += 1
            if mask_strings:
                end = index
                keep = {start}
                if end > start and text[end - 1] == quote:
                    keep.add(end - 1)
                blank(start, end, keep)
            continue
        index += 1
    return "".join(output)


def _strip_comments(text: str) -> str:
    return mask_noncode(text, mask_strings=False)


def _namespace_spans(text: str) -> list[tuple[int, int, str]]:
    spans: list[tuple[int, int, str]] = []
    locator = mask_noncode(text)
    for match in re.finditer(r"\bnamespace\s+([A-Za-z_]\w*)\s*\{", locator):
        open_brace = locator.find("{", match.start())
        try:
            _, end = extract_balanced(locator, open_brace, "{", "}")
        except GenerationError:
            continue
        spans.append((open_brace + 1, end - 1, match.group(1)))
    return spans


def parse_cpp_constants(text: str) -> dict[str, object]:
    """Parse the literal constexpr subset used by SpatialParamIDs.h."""
    locator = mask_noncode(text)
    namespaces = _namespace_spans(text)
    constants: dict[str, object] = {}
    bare: dict[str, list[object]] = {}

    declaration = re.compile(
        r"(?:inline\s+)?constexpr\s+"
        r"(?:const\s+char\s*\*|float|double|int|size_t|bool)\s+"
        r"([A-Za-z_]\w*)\s*(\[\s*\])?\s*=\s*",
        re.MULTILINE,
    )
    for match in declaration.finditer(locator):
        name = match.group(1)
        is_array = match.group(2) is not None
        expr_start = match.end()
        if is_array:
            open_brace = locator.find("{", expr_start)
            if open_brace < 0:
                raise GenerationError(f"constant {name}: expected array initializer")
            _, end = extract_balanced(locator, open_brace, "{", "}")
            body = text[open_brace + 1 : end - 1]
            if ";" not in locator[end : end + 8]:
                raise GenerationError(f"constant {name}: missing terminator")
            value = [resolve_cpp_literal(field, constants) for field in split_top_level(body)]
        else:
            semicolon = locator.find(";", expr_start)
            if semicolon < 0:
                raise GenerationError(f"constant {name}: missing terminator")
            value = resolve_cpp_literal(text[expr_start:semicolon], constants)

        enclosing = [
            (start, namespace)
            for start, end, namespace in namespaces
            if start <= match.start() < end
        ]
        enclosing.sort(key=lambda entry: entry[0])
        qualified = "::".join([namespace for _, namespace in enclosing] + [name])
        constants[qualified] = value
        constants.setdefault(name, value)
        bare.setdefault(name, []).append(value)

    for name, values in bare.items():
        if len(values) == 1:
            constants[name] = values[0]
    return constants


def resolve_cpp_literal(expr: str, constants: dict[str, object]) -> object:
    value = _strip_comments(expr).strip()
    while value.startswith("(") and value.endswith(")"):
        try:
            body, end = extract_balanced(value, 0, "(", ")")
        except GenerationError:
            break
        if end != len(value):
            break
        value = body.strip()
    if value in constants:
        return constants[value]
    if value == "true":
        return True
    if value == "false":
        return False
    utf8_string = re.fullmatch(
        r"(?:juce::)?String\s*\(\s*(?:juce::)?CharPointer_UTF8\s*\((.*)\)\s*\)",
        value,
        re.DOTALL,
    )
    if utf8_string:
        resolved = resolve_cpp_literal(utf8_string.group(1), constants)
        if isinstance(resolved, str):
            return resolved
        raise GenerationError(f"non-string UTF-8 literal: {expr.strip()}")
    if re.fullmatch(r'"(?:\\.|[^"\\])*"', value):
        try:
            return json.loads(value)
        except json.JSONDecodeError as error:
            raise GenerationError(f"invalid C++ string literal: {expr}") from error
    number = re.fullmatch(
        r"([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)(?:[fFuUlL]*)",
        value,
    )
    if number:
        numeric = number.group(1)
        if "." in numeric or "e" in numeric.lower():
            return float(numeric)
        return int(numeric)
    raise GenerationError(f"unsupported C++ literal: {expr.strip()}")


def _guard_at_offsets(source: str) -> tuple[dict[int, tuple[str, ...]], list[int]]:
    active: list[str] = []
    guards: dict[int, tuple[str, ...]] = {}
    line_offsets: list[int] = []
    offset = 0
    code = mask_noncode(source)
    for line in code.splitlines(keepends=True):
        line_offsets.append(offset)
        stripped = line.strip()
        if stripped.startswith("#if"):
            condition = stripped[3:].split("//", 1)[0].strip()
            active.append(condition)
        guards[offset] = tuple(active)
        if stripped.startswith("#endif"):
            if not active:
                raise GenerationError("unmatched #endif in parameter source")
            active.pop()
        elif stripped.startswith("#else") or stripped.startswith("#elif"):
            raise GenerationError("unknown parameter guard: conditional branch")
        offset += len(line)
    if active:
        raise GenerationError("unterminated parameter guard")
    return guards, line_offsets


def _guards_for_position(
    guard_map: dict[int, tuple[str, ...]], line_offsets: list[int], position: int
) -> tuple[str, ...]:
    selected = line_offsets[0] if line_offsets else 0
    for offset in line_offsets:
        if offset > position:
            break
        selected = offset
    return guard_map.get(selected, ())


def _parse_string_array(expr: str, arrays: dict[str, list[str]]) -> list[str]:
    clean = _strip_comments(expr).strip()
    if clean in arrays:
        return list(arrays[clean])
    match = re.match(r"(?:juce::)?StringArray\s*([({])", clean)
    if match:
        opener = match.group(1)
        closer = ")" if opener == "(" else "}"
        start = clean.find(opener, match.start())
        body, end = extract_balanced(clean, start, opener, closer)
        if clean[end:].strip():
            raise GenerationError(f"unsupported choice expression: {expr.strip()}")
        values = [resolve_cpp_literal(field, {}) for field in split_top_level(body)]
        if not all(isinstance(value, str) for value in values):
            raise GenerationError(f"non-string choice value: {expr.strip()}")
        return [str(value) for value in values]
    raise GenerationError(f"unsupported choice expression: {expr.strip()}")


def _parse_range(
    expr: str,
    constants: dict[str, object],
    named_ranges: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    clean = _strip_comments(expr).strip()
    if clean in named_ranges:
        return dict(named_ranges[clean])
    match = re.match(r"(?:juce::)?NormalisableRange\s*<\s*float\s*>\s*([({])", clean)
    if match is None:
        raise GenerationError(f"unsupported parameter range: {expr.strip()}")
    opener = match.group(1)
    closer = ")" if opener == "(" else "}"
    start = clean.find(opener, match.start())
    body, end = extract_balanced(clean, start, opener, closer)
    if clean[end:].strip():
        raise GenerationError(f"unsupported parameter range: {expr.strip()}")
    fields = split_top_level(body)
    if not 2 <= len(fields) <= 4:
        raise GenerationError(f"malformed parameter range: {expr.strip()}")
    values = [resolve_cpp_literal(field, constants) for field in fields]
    if not all(isinstance(value, (int, float)) and not isinstance(value, bool) for value in values):
        raise GenerationError(f"non-numeric parameter range: {expr.strip()}")
    result: dict[str, Any] = {"min": values[0], "max": values[1]}
    if len(values) >= 3:
        result["interval"] = values[2]
    if len(values) == 4:
        result["skew"] = values[3]
    return result


def _parameter_id(expr: str, symbols: dict[str, str]) -> str:
    match = re.search(r"(?:juce::)?ParameterID\s*\{", expr)
    if match is None:
        raise GenerationError(f"missing ParameterID: {expr.strip()}")
    start = expr.find("{", match.start())
    body, _ = extract_balanced(expr, start, "{", "}")
    fields = split_top_level(body)
    if len(fields) < 1:
        raise GenerationError(f"empty ParameterID: {expr.strip()}")
    identifier_expr = _strip_comments(fields[0]).strip()
    try:
        value = resolve_cpp_literal(identifier_expr, symbols)
    except GenerationError as error:
        raise GenerationError(
            f"unresolved parameter ID symbol: {identifier_expr}"
        ) from error
    if not isinstance(value, str):
        raise GenerationError(f"parameter ID is not a string: {identifier_expr}")
    return value


def _formatter_from_arguments(arguments: list[str]) -> str | None:
    attributes = ", ".join(arguments)
    match = re.search(r"\.withStringFromValueFunction\s*\(", attributes)
    if match is None:
        return None
    start = attributes.find("(", match.start())
    body, _ = extract_balanced(attributes, start, "(", ")")
    clean = _strip_comments(body).strip()
    if clean.startswith("["):
        return "inline_lambda"
    return clean or None


def parse_parameters(source: str, id_symbols: dict[str, str]) -> list[dict]:
    """Parse supported APVTS registrations in declaration order."""
    constants: dict[str, object] = dict(id_symbols)
    constants.update(parse_cpp_constants(source))
    code = mask_noncode(source)
    parameter_construction = re.compile(
        r"std::make_unique<\s*(?:juce::)?(?P<factory>AudioParameter[A-Za-z0-9_]+)\s*>\s*\("
        r"|\bnew\s+(?:juce::)?(?P<allocated>AudioParameter[A-Za-z0-9_]+)\s*\("
        r"|(?<![<\w])(?:juce::)?(?P<direct>AudioParameter(?:Bool|Choice|Float|Int))\s*[({]"
    )

    string_arrays: dict[str, list[str]] = {}
    array_pattern = re.compile(r"(?:juce::)?StringArray\s+([A-Za-z_]\w*)\s*([({])")
    for match in array_pattern.finditer(code):
        opener = match.group(2)
        closer = ")" if opener == "(" else "}"
        start = source.find(opener, match.start())
        body, _ = extract_balanced(source, start, opener, closer)
        try:
            values = [resolve_cpp_literal(field, constants) for field in split_top_level(body)]
        except GenerationError:
            continue
        if all(isinstance(value, str) for value in values):
            string_arrays[match.group(1)] = [str(value) for value in values]
        elif (
            len(values) == 2
            and isinstance(values[0], list)
            and all(isinstance(value, str) for value in values[0])
            and isinstance(values[1], int)
            and not isinstance(values[1], bool)
            and 0 <= values[1] <= len(values[0])
        ):
            # StringArray name (constantArray, count), as used for the shared
            # note-division labels in SpatialParamIDs.h.
            string_arrays[match.group(1)] = [str(value) for value in values[0][: values[1]]]

    for declaration in re.finditer(r"(?:juce::)?StringArray\s+([A-Za-z_]\w*)\s*;", code):
        name = declaration.group(1)
        add = re.search(
            rf"\b{re.escape(name)}\.add\s*\(\s*([A-Za-z_:]\w*(?:::\w+)*)\s*\[\s*i\s*\]\s*\)",
            code[declaration.end() :],
        )
        if add:
            symbol = add.group(1)
            resolved = constants.get(symbol)
            if isinstance(resolved, list) and all(isinstance(value, str) for value in resolved):
                string_arrays[name] = [str(value) for value in resolved]

    named_ranges: dict[str, dict[str, Any]] = {}
    range_pattern = re.compile(
        r"(?:juce::)?NormalisableRange\s*<\s*float\s*>\s+([A-Za-z_]\w*)\s*([({])"
    )
    for match in range_pattern.finditer(code):
        opener = match.group(2)
        closer = ")" if opener == "(" else "}"
        start = source.find(opener, match.start())
        body, _ = extract_balanced(source, start, opener, closer)
        parsed = _parse_range(
            f"juce::NormalisableRange<float>{opener}{body}{closer}", constants, {}
        )
        skew = re.search(
            rf"\b{re.escape(match.group(1))}\.setSkewForCentre\s*\(([^)]+)\)",
            code[match.end() :],
        )
        if skew:
            parsed["skew_centre"] = resolve_cpp_literal(skew.group(1), constants)
        named_ranges[match.group(1)] = parsed

    groups: dict[str, str] = {}
    group_order: dict[str, int] = {}
    group_pattern = re.compile(
        r"auto\s+([A-Za-z_]\w*)\s*=\s*std::make_unique<"
        r"(?:juce::)?AudioProcessorParameterGroup>\s*\("
    )
    for index, match in enumerate(group_pattern.finditer(code)):
        start = source.find("(", match.start())
        body, _ = extract_balanced(source, start, "(", ")")
        fields = split_top_level(body)
        if len(fields) < 2:
            raise GenerationError(f"malformed parameter group: {match.group(0)}")
        group_name = resolve_cpp_literal(fields[0], constants)
        if not isinstance(group_name, str):
            raise GenerationError(f"non-string parameter group: {fields[0]}")
        groups[match.group(1)] = group_name
        group_order[match.group(1)] = index

    guard_map, line_offsets = _guard_at_offsets(source)
    rows: list[dict] = []
    seen: set[str] = set()
    registration = re.compile(
        r"\b([A-Za-z_]\w*)\s*->\s*addChild\s*\(\s*"
        r"std::make_unique<\s*(?:juce::)?(AudioParameter[A-Za-z0-9_]+)\s*>\s*\("
    )
    supported = {"AudioParameterBool", "AudioParameterChoice", "AudioParameterFloat"}
    registration_index: dict[str, int] = {}
    consumed_constructions: set[int] = set()

    for match in registration.finditer(code):
        variable, parameter_type = match.groups()
        construction = parameter_construction.search(code, match.start(), match.end())
        if construction is None:
            raise GenerationError(
                f"unsupported parameter registration at line "
                f"{source.count(chr(10), 0, match.start()) + 1}: "
                f"{source.splitlines()[source.count(chr(10), 0, match.start())].strip()}"
            )
        consumed_constructions.add(construction.start())
        if variable not in groups:
            raise GenerationError(f"unknown parameter group variable: {variable}")
        if parameter_type not in supported:
            raise GenerationError(f"unsupported parameter constructor: {parameter_type}")
        open_paren = source.find("(", match.end() - 1)
        body, _ = extract_balanced(source, open_paren, "(", ")")
        fields = split_top_level(body)
        guards = _guards_for_position(guard_map, line_offsets, match.start())
        for guard in guards:
            if guard != "JUCE_DEBUG":
                raise GenerationError(f"unknown parameter guard: {guard}")
        build_scope = "debug" if "JUCE_DEBUG" in guards else "release"

        minimum_fields = {
            "AudioParameterBool": 3,
            "AudioParameterChoice": 4,
            "AudioParameterFloat": 4,
        }[parameter_type]
        if len(fields) < minimum_fields:
            raise GenerationError(
                f"malformed {parameter_type} registration: {body.strip()}"
            )
        identifier = _parameter_id(fields[0], id_symbols)
        if identifier in seen:
            raise GenerationError(f"duplicate parameter ID: {identifier}")
        seen.add(identifier)
        try:
            name = resolve_cpp_literal(fields[1], constants)
        except GenerationError as error:
            raise GenerationError(
                f"unresolved parameter name expression: {fields[1].strip()}"
            ) from error
        if not isinstance(name, str):
            raise GenerationError(f"parameter name is not a string: {fields[1]}")

        row: dict[str, Any] = {
            "id": identifier,
            "name": name,
            "type": parameter_type.removeprefix("AudioParameter").lower(),
            "group": groups[variable],
            "default": None,
            "range": None,
            "choices": None,
            "unit": None,
            "automatable": ".withAutomatable(false)" not in mask_noncode(body),
            "build_scope": build_scope,
        }
        if parameter_type == "AudioParameterBool":
            default = resolve_cpp_literal(fields[2], constants)
            if not isinstance(default, bool):
                raise GenerationError(f"invalid bool default: {fields[2].strip()}")
            row["default"] = default
            trailing = fields[3:]
        elif parameter_type == "AudioParameterChoice":
            row["choices"] = _parse_string_array(fields[2], string_arrays)
            default = resolve_cpp_literal(fields[3], constants)
            if not isinstance(default, int) or isinstance(default, bool):
                raise GenerationError(f"invalid choice default: {fields[3].strip()}")
            if default < 0 or default >= len(row["choices"]):
                raise GenerationError(f"choice default outside range: {identifier}")
            row["default"] = default
            trailing = fields[4:]
        else:
            parsed_range = _parse_range(fields[2], constants, named_ranges)
            default = resolve_cpp_literal(fields[3], constants)
            if not isinstance(default, (int, float)) or isinstance(default, bool):
                raise GenerationError(f"invalid float default: {fields[3].strip()}")
            if parsed_range["min"] > parsed_range["max"]:
                raise GenerationError(f"malformed parameter range: {identifier}")
            if default < parsed_range["min"] or default > parsed_range["max"]:
                raise GenerationError(f"default outside range: {identifier}")
            row["range"] = parsed_range
            row["default"] = default
            trailing = fields[4:]

        formatter = _formatter_from_arguments(trailing)
        if formatter is not None:
            row["display_formatter"] = formatter
        registration_index[identifier] = len(rows)
        rows.append(row)

    for construction in parameter_construction.finditer(code):
        if construction.start() in consumed_constructions:
            continue
        line_number = source.count("\n", 0, construction.start()) + 1
        source_lines = source.splitlines()
        context = source_lines[line_number - 1].strip() if source_lines else ""
        raise GenerationError(
            f"unsupported parameter registration at line {line_number}: {context}"
        )

    rows.sort(
        key=lambda row: (
            group_order[
                next(variable for variable, name in groups.items() if name == row["group"])
            ],
            registration_index[row["id"]],
        )
    )
    return rows


def _required_text(repo_root: Path, relative: str, field: str | None = None) -> str:
    path = repo_root / relative
    if not path.is_file():
        suffix = f" required field {field}" if field else ""
        raise GenerationError(f"{relative}: missing{suffix}")
    return path.read_text(encoding="utf-8")


def _source_rows(repo_root: Path, relatives: list[str]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for relative in relatives:
        path = repo_root / relative
        if not path.is_file():
            raise GenerationError(f"{relative}: source file missing")
        rows.append(
            {
                "path": relative,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
        )
    return rows


def parse_parameter_manifest(repo_root: Path) -> dict:
    sources = ["plugin/Source/PluginProcessor.cpp", "plugin/Source/SpatialParamIDs.h"]
    processor = _required_text(repo_root, sources[0])
    ids_text = _required_text(repo_root, sources[1])
    constants = parse_cpp_constants(ids_text)
    body = extract_function_body(processor, "SpatialEchoAudioProcessor::params")
    # The same source-of-truth header carries ID strings, display strings,
    # defaults, and range endpoints. parse_parameters resolves that verified
    # literal table and still rejects every unsupported expression.
    try:
        items = parse_parameters(body, constants)  # type: ignore[arg-type]
    except GenerationError as error:
        raise GenerationError(f"{sources[0]}: {error}") from error
    release_count = sum(item["build_scope"] == "release" for item in items)
    debug_count = sum(item["build_scope"] == "debug" for item in items)
    groups: list[str] = []
    for item in items:
        if item["group"] not in groups:
            groups.append(item["group"])
    return {
        "schema_version": 1,
        "sources": _source_rows(repo_root, sources),
        "summary": {
            "release_count": release_count,
            "debug_count": debug_count,
            "total_count": len(items),
            "group_count": len(groups),
            "groups": groups,
        },
        "items": items,
    }
