"""PlatformIO pre-build banner for the single fleet-wide network selector."""

Import("env")  # type: ignore[name-defined]  # PlatformIO/SCons injects this


def _profile_selector():
    parsed = env.ParseFlags(env.get("BUILD_FLAGS", []))  # type: ignore[name-defined]
    for define in parsed.get("CPPDEFINES", []):
        if isinstance(define, (tuple, list)) and define[0] == "SMARTFIRES_NETWORK_PROFILE":
            return int(define[1])
        if isinstance(define, str) and define.startswith("SMARTFIRES_NETWORK_PROFILE="):
            return int(define.split("=", 1)[1])
    return None


selector = _profile_selector()
if selector not in (7, 9, 10, 12):
    raise RuntimeError(
        "SMARTFIRES_NETWORK_PROFILE must be explicitly set to 7, 9, 10, or 12"
    )

print("=" * 68)
print(f" SmartFires fleet network profile: SF{selector}")
print(" Build and flash the matching base, every node, and the sniffer together.")
print("=" * 68)
