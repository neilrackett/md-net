#!/usr/bin/env bash
#
# tools/reset_template.sh
#
# Reset the template to a clean starting point for a new app:
#   - strip the bundled demos and install the bare hello_text app
#   - generate a fresh app UUID into uuid.txt
#   - reset version.txt (and the rp/ + target/ copies) to v0.0.1
#   - reset CHANGELOG.md and README.md to stubs
#
# Run it from anywhere in a template checkout.
#
# Note: this replaces the template's own README.md -- the API guide it
# holds stays available in CLAUDE.md, programming.md and the
# framebuffer-app skill, and git restores it (see Revert below).
#
# Revert with:  rm -rf rp && mv rp.bak rp
#               git checkout version.txt rp/version.txt target/version.txt \
#                            CHANGELOG.md README.md
#               rm -f uuid.txt
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

RESET_VERSION="v0.0.1"

# Strip the demos first. apply.sh refuses to clobber an existing rp.bak, so
# running it up front means that failure aborts before anything else is reset.
"$ROOT/examples/hello_text/apply.sh"

echo "Generating a new app UUID ..."
if command -v uuidgen >/dev/null 2>&1; then
    APP_UUID="$(uuidgen | tr '[:upper:]' '[:lower:]')"
else
    APP_UUID="$(python3 -c 'import uuid; print(uuid.uuid4())')"
fi
printf "%s\n" "$APP_UUID" > uuid.txt

echo "Resetting version to $RESET_VERSION ..."
printf "%s\n" "$RESET_VERSION" > version.txt
"$SCRIPT_DIR/bump_version.sh" --repo-root "$ROOT" --no-bump

echo "Resetting CHANGELOG.md ..."
cat > CHANGELOG.md <<EOF
# Changelog

## $RESET_VERSION

Initial release.
EOF

# Quoted heredoc: the stub contains markdown backticks, which an unquoted
# one would run as command substitution.
echo "Resetting README.md ..."
cat > README.md <<'EOF'
# <APP_NAME>

An Atari ST microfirmware app built on the md-framebuffer-template.

## Build

```bash
make build     # release build; uses the app UUID from uuid.txt
make debug     # debug build (bumps the patch version)
```

Flash the resulting `dist/<uuid>-<version>.uf2` to the Pico.
EOF

cat <<EOF

Done. The template is reset for a new app.

  App UUID:  $APP_UUID (uuid.txt)
  Version:   $RESET_VERSION
  Demos:     stripped -- the original rp/ is backed up in rp.bak

  Build:   make debug
  Revert:  rm -rf rp && mv rp.bak rp
           git checkout version.txt rp/version.txt target/version.txt \\
                        CHANGELOG.md README.md
           rm -f uuid.txt
EOF
