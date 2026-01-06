#!/bin/bash
# Generate .clangd configuration for the current directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cat > "$SCRIPT_DIR/.clangd" << EOF
CompileFlags:
  Add:
    - -xc
    - -I$SCRIPT_DIR
    - -I$SCRIPT_DIR/include
    - -I$SCRIPT_DIR/include/lib
    - -I$SCRIPT_DIR/include/lib/kernel
    - -DUSERPROG
    - -std=gnu17
  Remove:
    - -m*
    - -f*sanitize*
EOF

echo "Generated .clangd with paths relative to: $SCRIPT_DIR"
