"""CodeWarrior (Metrowerks) C++ names, demangled.

    from cwdemangle import demangle
    demangle("Animate__Q34nw4r3lyt18AnimTransformBasicFUlPQ34nw4r3lyt4Pane")
      -> "nw4r::lyt::AnimTransformBasic::Animate(unsigned long, nw4r::lyt::Pane*)"
    demangle(..., params=False) -> "nw4r::lyt::AnimTransformBasic::Animate"

Maps published for Wii games keep the compiler's mangled names; ours are
demangled. Comparing the two needs one form, and overloads need the parameter
list: Animate(u32, Pane*) and Animate(u32, Material*) are different functions
with one name. Returns None for what is not a mangled C++ name.

The scheme: `name__` then the qualifying class (`<len><id>` or `Q<n>` of them),
then `C` for a const method, then `F` and the parameter types. Types: `C`
const, `P` pointer, `R` reference, `U`/`S` unsigned/signed, `A<n>_` array,
`F...` function, `M<class>` member pointer, a class as above, and the
builtins below. Templates are part of an identifier's length.
"""

BUILTIN = {
    "v": "void", "b": "bool", "c": "char", "s": "short", "i": "int", "l": "long",
    "x": "long long", "f": "float", "d": "double", "r": "long double", "w": "wchar_t", "e": "...",
}



class _Bad(Exception):
    pass


def _number(s, i):
    start = i
    while i < len(s) and s[i].isdigit():
        i += 1
    if start == i:
        raise _Bad()
    return int(s[start:i]), i


def _identifier(s, i):
    length, i = _number(s, i)
    if i + length > len(s):
        raise _Bad()
    return s[i:i + length], i + length


def _qualified(s, i):
    """A class name: `<len><id>` or `Q<n>` of them. Returns (parts, i)."""
    if s[i] == "Q":
        count = int(s[i + 1])
        i += 2
        parts = []
        for _ in range(count):
            part, i = _identifier(s, i)
            parts.append(part)
        return parts, i
    part, i = _identifier(s, i)
    return [part], i


def _type(s, i):
    """One parameter type. Returns (text, i)."""
    prefix = []
    while i < len(s) and s[i] in "CPRUS":
        prefix.append(s[i])
        i += 1
    if i >= len(s):
        raise _Bad()
    c = s[i]
    if c in BUILTIN:
        base, i = BUILTIN[c], i + 1
    elif c.isdigit() or c == "Q":
        parts, i = _qualified(s, i)
        base = "::".join(parts)
    elif c == "A":
        length, i = _number(s, i + 1)
        if s[i] != "_":
            raise _Bad()
        inner, i = _type(s, i + 1)
        base = f"{inner}[{length}]"
    elif c == "F":
        params, i = _params(s, i + 1)
        if i < len(s) and s[i] == "_":
            ret, i = _type(s, i + 1)
        else:
            ret = "void"
        base = f"{ret} (*)({params})"
    elif c == "M":
        parts, i = _qualified(s, i + 1)
        inner, i = _type(s, i)
        base = f"{inner} {'::'.join(parts)}::*"
    else:
        raise _Bad()
    # read right to left: PCc is pointer to const char
    text = base
    for p in reversed(prefix):
        if p == "C":
            text = f"const {text}" if not text.endswith(("*", "&")) else f"{text} const"
        elif p == "P":
            text += "*"
        elif p == "R":
            text += "&"
        elif p == "U":
            text = f"unsigned {text}"
        elif p == "S":
            text = f"signed {text}"
    return text, i


def _params(s, i):
    types = []
    while i < len(s) and s[i] != "_":
        text, i = _type(s, i)
        types.append(text)
    if types == ["void"]:
        types = []
    return ", ".join(types), i


def demangle(name, params=True):
    # the split is at the last `__` that a class or F follows; a name may itself
    # start with underscores (__ct, __dt, __as) or carry them (@unnamed@...)
    for cut in range(len(name) - 2, 0, -1):
        if name[cut:cut + 2] != "__" or cut == 0:
            continue
        base, rest = name[:cut], name[cut + 2:]
        if not rest or not (rest[0].isdigit() or rest[0] in "QCF"):
            continue
        try:
            scope = []
            i = 0
            if rest[0].isdigit() or rest[0] == "Q":
                scope, i = _qualified(rest, 0)
            const = False
            if i < len(rest) and rest[i] == "C":
                const, i = True, i + 1
            if i < len(rest) and rest[i] == "F":
                args, i = _params(rest, i + 1)
                if i < len(rest) and rest[i] == "_":  # (a template's return type)
                    _, i = _type(rest, i + 1)
            elif i == len(rest) and scope:
                args = None  # static data or a function the map left unqualified
            else:
                raise _Bad()
            if i != len(rest):
                raise _Bad()
        except (_Bad, IndexError, ValueError):
            continue
        # (constructors and destructors stay __ct and __dt, as our maps spell them)
        text = "::".join(scope + [base]) if scope else base
        if params and args is not None:
            text += f"({args})" + (" const" if const else "")
        return text
    return None
