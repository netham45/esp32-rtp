#!/usr/bin/env python3
"""
Auto-increment version script for ESP32 RTP firmware
Increments the build number (or patch) on each build
"""

import re
import sys
from pathlib import Path

def increment_version(version_file, increment_type='build'):
    """
    Increment version in version.h file

    Args:
        version_file: Path to version.h
        increment_type: 'build', 'patch', 'minor', or 'major'
    """
    version_path = Path(version_file)

    if not version_path.exists():
        print(f"Error: {version_file} not found", file=sys.stderr)
        return False

    # Read the current file
    content = version_path.read_text(encoding='utf-8')

    # Patterns to match version components
    patterns = {
        'major': (r'(#define FIRMWARE_VERSION_MAJOR\s+)(\d+)', 'FIRMWARE_VERSION_MAJOR'),
        'minor': (r'(#define FIRMWARE_VERSION_MINOR\s+)(\d+)', 'FIRMWARE_VERSION_MINOR'),
        'patch': (r'(#define FIRMWARE_VERSION_PATCH\s+)(\d+)', 'FIRMWARE_VERSION_PATCH'),
        'build': (r'(#define FIRMWARE_BUILD_NUMBER\s+)(\d+)', 'FIRMWARE_BUILD_NUMBER'),
    }

    # Extract current version numbers
    versions = {}
    for key, (pattern, _) in patterns.items():
        match = re.search(pattern, content)
        if match:
            versions[key] = int(match.group(2))
        else:
            print(f"Warning: Could not find {key} version", file=sys.stderr)
            versions[key] = 0

    # Increment the specified version component
    if increment_type in versions:
        versions[increment_type] += 1

        # If incrementing major, reset minor and patch
        if increment_type == 'major':
            versions['minor'] = 0
            versions['patch'] = 0
        # If incrementing minor, reset patch
        elif increment_type == 'minor':
            versions['patch'] = 0
    else:
        print(f"Error: Unknown increment type '{increment_type}'", file=sys.stderr)
        return False

    # Update the content
    for key, (pattern, name) in patterns.items():
        content = re.sub(pattern, f'\\g<1>{versions[key]}', content)

    # Update version strings
    version_string = f'{versions["major"]}.{versions["minor"]}.{versions["patch"]}-{versions["build"]}'
    content = re.sub(
        r'(#define FIRMWARE_VERSION_STRING\s+")([^"]+)(")',
        f'\\g<1>{version_string}\\g<3>',
        content
    )

    full_version = f'ESP32 RTP Transciever v{version_string}'
    content = re.sub(
        r'(#define FIRMWARE_VERSION_FULL\s+")([^"]+)(")',
        f'\\g<1>{full_version}\\g<3>',
        content
    )

    # Write back the file
    version_path.write_text(content, encoding='utf-8')

    print(f"Version incremented: {increment_type} -> {version_string}")
    return True

if __name__ == '__main__':
    # Default to incrementing build number
    increment_type = 'build'

    # Allow override via command line argument
    if len(sys.argv) > 2:
        increment_type = sys.argv[2]

    version_file = sys.argv[1] if len(sys.argv) > 1 else 'version.h'

    success = increment_version(version_file, increment_type)
    sys.exit(0 if success else 1)