#!/usr/bin/env python3
"""Check all imports against XNU exports and the bundle's KPI dependencies."""

from __future__ import annotations

import subprocess
import sys
import plistlib
from collections import Counter
from pathlib import Path


def main() -> int:
	if len(sys.argv) not in (3, 4):
		print(f"usage: {sys.argv[0]} <kext-binary> <xnu-all-kpi.exp> "
		    "[xnu-symbolsets.plist]",
		    file=sys.stderr)
		return 2

	binary = Path(sys.argv[1])
	exports_file = Path(sys.argv[2])
	symbolsets_file = (Path(sys.argv[3]) if len(sys.argv) == 4 else
	    exports_file.with_name("symbolsets.plist"))
	info_file = binary.parent.parent / "Info.plist"
	if not all(path.is_file() for path in
	    (binary, exports_file, symbolsets_file, info_file)):
		print("error: binary, Info.plist, or generated XNU KPI metadata missing",
		    file=sys.stderr)
		return 2

	undefined_output = subprocess.check_output(["nm", "-u", str(binary)],
	    text=True)
	imports = {line.strip().split()[-1] for line in undefined_output.splitlines()
	    if line.strip()}
	exports = set()
	for line in exports_file.read_text().splitlines():
		line = line.strip()
		if line and not line.startswith("#"):
			exports.add(line.split()[0])

	missing = sorted(imports - exports)
	if missing:
		print(f"KPI audit failed: {len(missing)} of {len(imports)} imports "
		    "are absent from the supplied XNU all-kpi.exp:")
		for symbol in missing:
			print(f"  {symbol}")
		return 1

	print(f"KPI audit passed: all {len(imports)} undefined imports appear in "
	    f"{exports_file}")
	try:
		metadata = plistlib.loads(symbolsets_file.read_bytes())
		libraries = plistlib.loads(info_file.read_bytes())["OSBundleLibraries"]
		symbolsets = metadata["SymbolsSets"]
		if not isinstance(libraries, dict) or not libraries or not symbolsets:
			raise ValueError("empty or invalid dependency metadata")
		providers = {}
		for symbolset in symbolsets:
			identifier = symbolset["CFBundleIdentifier"]
			for symbol in symbolset["Symbols"]:
				# Prefix entries occur in Private KPI sets; they do not grant
				# access unless that provider is a declared dependency.
				name = symbol.get("SymbolName")
				prefix = symbol.get("SymbolPrefix")
				if name is None and prefix is None:
					raise ValueError("symbol entry has no name or prefix")
				for imported in imports:
					if imported == name or (prefix is not None and
					    imported.startswith(prefix)):
						providers.setdefault(imported, set()).add(identifier)
	except (KeyError, TypeError, ValueError, plistlib.InvalidFileException) as error:
		print(f"Dependency audit failed: {error}", file=sys.stderr)
		return 1

	uncovered = sorted(name for name in imports if not
	    providers.get(name, set()).intersection(libraries))
	if uncovered:
		print(f"Dependency audit failed: {len(uncovered)} imports have no "
		    "provider in OSBundleLibraries:")
		for name in uncovered:
			print(f"  {name}: {', '.join(sorted(providers.get(name, set()))) or 'unknown provider'}")
		return 1
	counts = Counter(provider for name in imports for provider in
	    providers[name].intersection(libraries))
	print("Declared dependency audit passed: " + ", ".join(
	    f"{provider}={count}" for provider, count in sorted(counts.items())))
	print("This source export-list check is not a substitute for kmutil "
	    "binding against the live kernel collection.")
	return 0


if __name__ == "__main__":
	sys.exit(main())
