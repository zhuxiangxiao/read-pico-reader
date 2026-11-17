#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
from pathlib import Path
import runpy
root=Path(__file__).resolve().parents[1]
runpy.run_path(str(root/'tools/five_tab_navigation_host_test.py'),run_name='__main__')
loop=(root/'main/app/app_loop.c').read_text()
assert 'display_system_binary_set' not in loop, 'Run in the board-selected Pico host view'
